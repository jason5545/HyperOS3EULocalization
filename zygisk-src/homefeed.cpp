#include "homefeed.h"

#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include <sys/mman.h>
#include <sys/syscall.h>

#include "art_resolver.h"
#include "dobby.h"
#include "gen/obf_strings.h"
#include "lsplant.hpp"
#include "obfstr.h"

// 內嵌 dex 由 gen/hooker_dex.h 產生，但 xxd -i 產生的是非 static 全域定義；
// 由 dualwake.cpp 包含一次，這裡只 extern 引用，避免重複符號。
extern unsigned char hooker_dex[];
extern unsigned int hooker_dex_len;

// 本檔的 JNI / lsplant 輔助函式比照 dualwake.cpp 的實作；dualwake 有
// tombstone 級的 dlclose 教訓且正在服役，為最小侵入不抽出共用、各留一份。

// 比照 main.cpp：log 字串在 release 版編譯期移除（-DTAPLUS_DEBUG_LOG 才保留）。
#ifdef TAPLUS_DEBUG_LOG
#define LOG_TAG "HomeFeed"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) do {} while (0)
#define LOGW(...) do {} while (0)
#endif

namespace {

// 模組 id（module.prop）決定的安裝路徑；liblsplant.so 由 build.sh 打包進這裡。
// 路徑由 gen_obf_strings.py 編碼（gen/obf_strings.h），用時解到 stack、
// 用完抹除（見 obfstr.h）。

// dlsym 用的 v2 ABI 符號（與 vendor/lsplant 內的 lsplant.hpp 宣告對應）。
using LsplantInitFn = bool (*)(JNIEnv *, const lsplant::InitInfo &);
using LsplantHookFn = jobject (*)(JNIEnv *, jobject, jobject, jobject);
using LsplantIsHookedFn = bool (*)(JNIEnv *, jobject);
constexpr const char *kLsplantInitSym =
        "_ZN7lsplant2v24InitEP7_JNIEnvRKNS0_8InitInfoE";
constexpr const char *kLsplantHookSym =
        "_ZN7lsplant2v24HookEP7_JNIEnvP8_jobjectS4_S4_";
constexpr const char *kLsplantIsHookedSym =
        "_ZN7lsplant2v28IsHookedEP7_JNIEnvP8_jobject";

uint8_t *g_lsplant_bytes = nullptr;
size_t g_lsplant_size = 0;

void *g_lsplant_handle = nullptr;      // dlopen 後永不 dlclose：hook 存活期間需要
jobject g_hooker_ref = nullptr;        // hooker 實例的額外 GC root
jobject g_hooker_ref2 = nullptr;       // MinusScreenHooker 的 GC root
jobject g_hooker_ref3 = nullptr;       // WidgetPickerHooker 的 GC root

bool clearException(JNIEnv *env, const char *what) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    LOGW("%s: JNI exception cleared", what);
    return true;
}

// 等 ActivityThread.currentApplication() 可用；回傳 local ref（呼叫端負責刪）。
// settle 0：LauncherAssistantCompat 的 CLIENT_ID_BASE static final 在類首次
// 載入時讀 prop，必須趕在桌面初始化 minus screen 之前裝好 hook。
jobject waitForApplication(JNIEnv *env) {
    jclass activity_thread = env->FindClass("android/app/ActivityThread");
    if (clearException(env, "ActivityThread") || !activity_thread) return nullptr;
    jmethodID current_application = env->GetStaticMethodID(
            activity_thread, "currentApplication", "()Landroid/app/Application;");
    if (clearException(env, "currentApplication") || !current_application) {
        env->DeleteLocalRef(activity_thread);
        return nullptr;
    }

    jobject application = nullptr;
    for (int attempt = 0; attempt < 1000 && !application; ++attempt) {
        application = env->CallStaticObjectMethod(activity_thread, current_application);
        if (clearException(env, "currentApplication call")) application = nullptr;
        if (!application) usleep(10000);
    }
    env->DeleteLocalRef(activity_thread);
    return application;
}

jclass loadClassWith(JNIEnv *env, jobject class_loader, const char *name) {
    jclass cl_class = env->FindClass("java/lang/ClassLoader");
    if (clearException(env, "ClassLoader") || !cl_class) return nullptr;
    jmethodID load_class = env->GetMethodID(cl_class, "loadClass",
                                            "(Ljava/lang/String;)Ljava/lang/Class;");
    if (clearException(env, "loadClass") || !load_class) {
        env->DeleteLocalRef(cl_class);
        return nullptr;
    }
    jstring str = env->NewStringUTF(name);
    jobject cls = str ? env->CallObjectMethod(class_loader, load_class, str) : nullptr;
    if (clearException(env, name)) cls = nullptr;
    if (str) env->DeleteLocalRef(str);
    env->DeleteLocalRef(cl_class);
    return static_cast<jclass>(cls);
}

// 用內嵌 dex 建 InMemoryDexClassLoader（parent = app classloader）。
jobject makeDexLoader(JNIEnv *env, jobject application) {
    jclass imcl = env->FindClass("dalvik/system/InMemoryDexClassLoader");
    if (clearException(env, "InMemoryDexClassLoader") || !imcl) return nullptr;
    jmethodID ctor = env->GetMethodID(imcl, "<init>",
            "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    if (clearException(env, "InMemoryDexClassLoader.<init>") || !ctor) {
        env->DeleteLocalRef(imcl);
        return nullptr;
    }

    jclass app_class = env->GetObjectClass(application);
    jmethodID get_class_loader = app_class
            ? env->GetMethodID(app_class, "getClassLoader", "()Ljava/lang/ClassLoader;")
            : nullptr;
    jobject app_loader = (app_class && get_class_loader)
            ? env->CallObjectMethod(application, get_class_loader) : nullptr;
    if (clearException(env, "getClassLoader")) app_loader = nullptr;

    jobject buffer = env->NewDirectByteBuffer(
            const_cast<unsigned char *>(hooker_dex), hooker_dex_len);
    jobject loader = (buffer && app_loader)
            ? env->NewObject(imcl, ctor, buffer, app_loader) : nullptr;
    if (clearException(env, "new InMemoryDexClassLoader")) loader = nullptr;

    if (buffer) env->DeleteLocalRef(buffer);
    if (app_loader) env->DeleteLocalRef(app_loader);
    if (app_class) env->DeleteLocalRef(app_class);
    env->DeleteLocalRef(imcl);
    return loader;
}

// LSPlant 需要的 inline hooker：用 Dobby 實作。
void *inlineHooker(void *target, void *hooker) {
    dobby_dummy_func_t backup = nullptr;
    if (DobbyHook(target, reinterpret_cast<dobby_dummy_func_t>(hooker),
                  &backup) == RS_SUCCESS) {
        return reinterpret_cast<void *>(backup);
    }
    return nullptr;
}

bool inlineUnhooker(void *target) {
    return DobbyDestroy(target) == RS_SUCCESS;
}

// preAppSpecialize 階段讀入的 liblsplant.so，在這裡寫進 memfd 再 dlopen。
void *dlopenLsplantFromMemory() {
    if (!g_lsplant_bytes || g_lsplant_size == 0) return nullptr;

    int fd = static_cast<int>(syscall(__NR_memfd_create, "lsplant", 0));
    if (fd < 0) {
        LOGW("memfd_create failed: %s", strerror(errno));
        return nullptr;
    }
    size_t written = 0;
    while (written < g_lsplant_size) {
        ssize_t n = write(fd, g_lsplant_bytes + written, g_lsplant_size - written);
        if (n <= 0) {
            LOGW("memfd write failed: %s", strerror(errno));
            close(fd);
            return nullptr;
        }
        written += static_cast<size_t>(n);
    }
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    void *handle = dlopen(path, RTLD_NOW);
    if (!handle) LOGW("dlopen %s failed: %s", path, dlerror());
    close(fd);
    return handle;
}

// 在 com.miui.home 內對 LauncherAssistantCompat.newInstance 裝第二個 lsplant
// hook。所有查找失敗都只記 log 後返回，絕不讓例外逸出到 app。
void installMinusScreenHook(JNIEnv *env, jobject application, jobject loader,
                            LsplantHookFn lsplant_hook,
                            LsplantIsHookedFn lsplant_is_hooked) {
    jclass hooker_cls = loadClassWith(env, loader, "jrc.homefeed.MinusScreenHooker");
    if (!hooker_cls) {
        LOGW("MiuiHome: minus-screen hooker class unavailable, skip");
        return;
    }

    jmethodID default_ctor = env->GetMethodID(hooker_cls, "<init>", "()V");
    jobject hooker = default_ctor ? env->NewObject(hooker_cls, default_ctor) : nullptr;
    if (clearException(env, "minus hooker new") || !hooker) {
        env->DeleteLocalRef(hooker_cls);
        return;
    }

    jmethodID callback = env->GetMethodID(
            hooker_cls, "callback", "([Ljava/lang/Object;)Ljava/lang/Object;");
    jobject callback_method = callback
            ? env->ToReflectedMethod(hooker_cls, callback, JNI_FALSE) : nullptr;
    if (clearException(env, "minus callback method") || !callback_method) {
        env->DeleteLocalRef(hooker);
        env->DeleteLocalRef(hooker_cls);
        return;
    }

    // 目標在 app classpath：用 app 的 classloader 載入。
    jclass app_class = env->GetObjectClass(application);
    jmethodID get_class_loader = app_class
            ? env->GetMethodID(app_class, "getClassLoader", "()Ljava/lang/ClassLoader;")
            : nullptr;
    jobject app_loader = (app_class && get_class_loader)
            ? env->CallObjectMethod(application, get_class_loader) : nullptr;
    if (clearException(env, "minus app classloader")) app_loader = nullptr;

    jclass target_cls = app_loader
            ? loadClassWith(env, app_loader,
                            "com.miui.home.launcher.LauncherAssistantCompat")
            : nullptr;
    jmethodID target_mid = target_cls
            ? env->GetStaticMethodID(
                      target_cls, "newInstance",
                      "(Lcom/miui/home/launcher/BaseLauncher;)"
                      "Lcom/miui/home/launcher/LauncherAssistantCompat;")
            : nullptr;
    jobject target_method = (target_cls && target_mid)
            ? env->ToReflectedMethod(target_cls, target_mid, JNI_TRUE) : nullptr;
    if (clearException(env, "minus target lookup") || !target_method) {
        LOGW("MiuiHome: newInstance target missing, skip");
        if (target_cls) env->DeleteLocalRef(target_cls);
        if (app_loader) env->DeleteLocalRef(app_loader);
        if (app_class) env->DeleteLocalRef(app_class);
        env->DeleteLocalRef(hooker);
        env->DeleteLocalRef(hooker_cls);
        return;
    }
    env->DeleteLocalRef(target_cls);
    if (app_loader) env->DeleteLocalRef(app_loader);
    if (app_class) env->DeleteLocalRef(app_class);

    jobject backup = lsplant_hook(env, target_method, hooker, callback_method);
    if (clearException(env, "minus lsplant Hook") || !backup) {
        LOGW("MiuiHome: minus-screen lsplant Hook returned null");
        env->DeleteLocalRef(target_method);
        env->DeleteLocalRef(hooker);
        env->DeleteLocalRef(hooker_cls);
        return;
    }

    jfieldID backup_field = env->GetFieldID(
            hooker_cls, "backup", "Ljava/lang/reflect/Method;");
    if (clearException(env, "minus backup field") || !backup_field) {
        env->DeleteLocalRef(target_method);
        env->DeleteLocalRef(hooker);
        env->DeleteLocalRef(hooker_cls);
        return;
    }
    env->SetObjectField(hooker, backup_field, backup);
    if (clearException(env, "minus backup set")) {
        env->DeleteLocalRef(target_method);
        env->DeleteLocalRef(hooker);
        env->DeleteLocalRef(hooker_cls);
        return;
    }

    const bool hooked = lsplant_is_hooked(env, target_method);
    LOGI("MiuiHome minus-screen hook installed (IsHooked=%d)", hooked ? 1 : 0);
    g_hooker_ref2 = env->NewGlobalRef(hooker);
    env->DeleteLocalRef(target_method);
    env->DeleteLocalRef(hooker);
    env->DeleteLocalRef(hooker_cls);
}

// 在 com.miui.home 內對 WidgetManagerUtils.gotoPicker 裝第三個 lsplant
// hook：「小工具」按鈕在 EU（無 com.mi.globalminusscreen）永遠退回內建清單頁，
// 由 jrc.homefeed.WidgetPickerHooker 改導 personalassistant 的小部件中心。
// 所有查找失敗都只記 log 後返回，絕不讓例外逸出到 app。
void installWidgetPickerHook(JNIEnv *env, jobject application, jobject loader,
                             LsplantHookFn lsplant_hook,
                             LsplantIsHookedFn lsplant_is_hooked) {
    jclass hooker_cls = loadClassWith(env, loader, "jrc.homefeed.WidgetPickerHooker");
    if (!hooker_cls) {
        LOGW("MiuiHome: widget-picker hooker class unavailable, skip");
        return;
    }

    jmethodID default_ctor = env->GetMethodID(hooker_cls, "<init>", "()V");
    jobject hooker = default_ctor ? env->NewObject(hooker_cls, default_ctor) : nullptr;
    if (clearException(env, "picker hooker new") || !hooker) {
        env->DeleteLocalRef(hooker_cls);
        return;
    }

    jmethodID callback = env->GetMethodID(
            hooker_cls, "callback", "([Ljava/lang/Object;)Ljava/lang/Object;");
    jobject callback_method = callback
            ? env->ToReflectedMethod(hooker_cls, callback, JNI_FALSE) : nullptr;
    if (clearException(env, "picker callback method") || !callback_method) {
        env->DeleteLocalRef(hooker);
        env->DeleteLocalRef(hooker_cls);
        return;
    }

    // 目標在 app classpath：用 app 的 classloader 載入。
    jclass app_class = env->GetObjectClass(application);
    jmethodID get_class_loader = app_class
            ? env->GetMethodID(app_class, "getClassLoader", "()Ljava/lang/ClassLoader;")
            : nullptr;
    jobject app_loader = (app_class && get_class_loader)
            ? env->CallObjectMethod(application, get_class_loader) : nullptr;
    if (clearException(env, "picker app classloader")) app_loader = nullptr;

    jclass target_cls = app_loader
            ? loadClassWith(env, app_loader,
                            "com.miui.home.launcher.common.WidgetManagerUtils")
            : nullptr;
    jmethodID target_mid = target_cls
            ? env->GetStaticMethodID(
                      target_cls, "gotoPicker",
                      "(Lcom/miui/home/launcher/BaseLauncher;"
                      "Lcom/miui/home/model/api/ItemInfo;)V")
            : nullptr;
    jobject target_method = (target_cls && target_mid)
            ? env->ToReflectedMethod(target_cls, target_mid, JNI_TRUE) : nullptr;
    if (clearException(env, "picker target lookup") || !target_method) {
        LOGW("MiuiHome: gotoPicker target missing, skip");
        if (target_cls) env->DeleteLocalRef(target_cls);
        if (app_loader) env->DeleteLocalRef(app_loader);
        if (app_class) env->DeleteLocalRef(app_class);
        env->DeleteLocalRef(hooker);
        env->DeleteLocalRef(hooker_cls);
        return;
    }
    env->DeleteLocalRef(target_cls);
    if (app_loader) env->DeleteLocalRef(app_loader);
    if (app_class) env->DeleteLocalRef(app_class);

    jobject backup = lsplant_hook(env, target_method, hooker, callback_method);
    if (clearException(env, "picker lsplant Hook") || !backup) {
        LOGW("MiuiHome: widget-picker lsplant Hook returned null");
        env->DeleteLocalRef(target_method);
        env->DeleteLocalRef(hooker);
        env->DeleteLocalRef(hooker_cls);
        return;
    }

    jfieldID backup_field = env->GetFieldID(
            hooker_cls, "backup", "Ljava/lang/reflect/Method;");
    if (clearException(env, "picker backup field") || !backup_field) {
        env->DeleteLocalRef(target_method);
        env->DeleteLocalRef(hooker);
        env->DeleteLocalRef(hooker_cls);
        return;
    }
    env->SetObjectField(hooker, backup_field, backup);
    if (clearException(env, "picker backup set")) {
        env->DeleteLocalRef(target_method);
        env->DeleteLocalRef(hooker);
        env->DeleteLocalRef(hooker_cls);
        return;
    }

    const bool hooked = lsplant_is_hooked(env, target_method);
    LOGI("MiuiHome widget-picker hook installed (IsHooked=%d)", hooked ? 1 : 0);
    g_hooker_ref3 = env->NewGlobalRef(hooker);
    env->DeleteLocalRef(target_method);
    env->DeleteLocalRef(hooker);
    env->DeleteLocalRef(hooker_cls);
}

void *miuiHomeWorker(void *opaque) {
    auto *vm = static_cast<JavaVM *>(opaque);
    JNIEnv *env = nullptr;
    if (!vm || vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
        LOGW("MiuiHome: unable to attach worker");
        return nullptr;
    }

    bool installed = false;
    bool lsplant_init_called = false;
    jobject application = nullptr;
    jobject loader = nullptr;
    jclass hooker_cls = nullptr;
    jobject hooker = nullptr;
    jobject target_method = nullptr;

    do {
        application = waitForApplication(env);
        if (!application) {
            LOGW("MiuiHome: application unavailable, skip");
            break;
        }

        loader = makeDexLoader(env, application);
        hooker_cls = loader
                ? loadClassWith(env, loader, "jrc.homefeed.HomeRsaHooker")
                : nullptr;
        if (!hooker_cls) {
            LOGW("MiuiHome: hooker class unavailable, safe no-op");
            break;
        }

        // 版本保護：只在模組內建的 CN 桌面（750062529）上啟用；EU 桌面
        // 不讀 ro.com.miui.rsa，hook 對它沒有意義，安全停用。
        jmethodID should_install = env->GetStaticMethodID(
                hooker_cls, "shouldInstall", "(Landroid/content/Context;)Z");
        if (clearException(env, "shouldInstall") || !should_install) break;
        jboolean ok = env->CallStaticBooleanMethod(hooker_cls, should_install, application);
        if (clearException(env, "shouldInstall call")) { ok = JNI_FALSE; }
        if (!ok) break;  // 不吻合：安全停用

        // serviceVersion 保底：CN 版 launcherclient 的 service.api.version
        // 解析是 write-once static，開機遇上 GSA 暫態會永久卡在 legacy
        // attach。此修正與 lsplant 是否成功無關，先派生背景執行緒。
        jmethodID ensure_version = env->GetStaticMethodID(
                hooker_cls, "ensureServiceApiVersion", "(Landroid/content/Context;)V");
        if (ensure_version) {
            env->CallStaticVoidMethod(hooker_cls, ensure_version, application);
            clearException(env, "ensureServiceApiVersion call");
        } else {
            clearException(env, "ensureServiceApiVersion lookup");
        }

        if (!g_lsplant_bytes) {
            LOGW("MiuiHome: liblsplant payload missing, safe no-op");
            break;
        }
        g_lsplant_handle = dlopenLsplantFromMemory();
        if (!g_lsplant_handle) break;

        auto lsplant_init = reinterpret_cast<LsplantInitFn>(
                dlsym(g_lsplant_handle, kLsplantInitSym));
        auto lsplant_hook = reinterpret_cast<LsplantHookFn>(
                dlsym(g_lsplant_handle, kLsplantHookSym));
        auto lsplant_is_hooked = reinterpret_cast<LsplantIsHookedFn>(
                dlsym(g_lsplant_handle, kLsplantIsHookedSym));
        if (!lsplant_init || !lsplant_hook || !lsplant_is_hooked) {
            LOGW("MiuiHome: lsplant symbols missing");
            break;
        }

        auto *resolver = new ArtResolver();  // 跟進程同壽命，不釋放
        if (!resolver->init()) {
            LOGW("MiuiHome: libart symbol table unavailable");
            break;
        }

        // GarbageCollectCache → DoCollection 的 Android 16 回退理由同
        // dualwake.cpp；兩處若有一邊要改，另一邊也要改。
        constexpr const char *kGcCacheSym =
                "_ZN3art3jit12JitCodeCache19GarbageCollectCacheEPNS_6ThreadE";
        constexpr const char *kDoCollectionSym =
                "_ZN3art3jit12JitCodeCache12DoCollectionEPNS_6ThreadE";
        lsplant::InitInfo info = {
                inlineHooker,
                inlineUnhooker,
                [resolver](std::string_view symbol) -> void * {
                    const std::string name(symbol);
                    void *addr = resolver->resolve(name.c_str());
                    if (!addr && name == kGcCacheSym) {
                        addr = resolver->resolve(kDoCollectionSym);
                    }
                    return addr;
                },
                [resolver](std::string_view prefix) -> void * {
                    return resolver->resolvePrefix(std::string(prefix).c_str());
                },
        };
        lsplant_init_called = true;
        if (!lsplant_init(env, info)) {
            LOGW("MiuiHome: lsplant Init failed");
            break;
        }

        jmethodID default_ctor = env->GetMethodID(hooker_cls, "<init>", "()V");
        hooker = default_ctor ? env->NewObject(hooker_cls, default_ctor) : nullptr;
        if (clearException(env, "hooker new") || !hooker) break;

        jmethodID callback = env->GetMethodID(
                hooker_cls, "callback", "([Ljava/lang/Object;)Ljava/lang/Object;");
        jobject callback_method = callback
                ? env->ToReflectedMethod(hooker_cls, callback, JNI_FALSE) : nullptr;
        if (clearException(env, "callback method") || !callback_method) break;

        // 目標是 BOOTCLASSPATH 類，直接 FindClass；CN 桌面經自家
        // com.miui.launcher.utils.SystemProperties 反射呼叫的正是這個
        // 1-arg overload，反射路徑不可能被 AOT inline，hook 必中。
        jclass target_cls = env->FindClass("android/os/SystemProperties");
        jmethodID target_mid = target_cls
                ? env->GetStaticMethodID(target_cls, "get",
                                         "(Ljava/lang/String;)Ljava/lang/String;")
                : nullptr;
        if (clearException(env, "SystemProperties.get") || !target_cls || !target_mid) {
            LOGW("MiuiHome: prop target missing, safe no-op");
            if (target_cls) env->DeleteLocalRef(target_cls);
            break;
        }
        target_method = env->ToReflectedMethod(target_cls, target_mid, JNI_TRUE);
        env->DeleteLocalRef(target_cls);
        if (clearException(env, "SystemProperties.get reflect") || !target_method) break;

        jobject backup = lsplant_hook(env, target_method, hooker, callback_method);
        if (clearException(env, "lsplant Hook") || !backup) {
            LOGW("MiuiHome: lsplant Hook returned null");
            break;
        }

        jfieldID backup_field = env->GetFieldID(
                hooker_cls, "backup", "Ljava/lang/reflect/Method;");
        if (clearException(env, "backup field") || !backup_field) break;
        env->SetObjectField(hooker, backup_field, backup);
        if (clearException(env, "backup set")) break;

        const bool hooked = lsplant_is_hooked(env, target_method);
        LOGI("MiuiHome rsa hook installed (IsHooked=%d)", hooked ? 1 : 0);
        g_hooker_ref = env->NewGlobalRef(hooker);
        installed = true;

        // 第二個 hook（同進程、同 lsplant）：資訊助手模式改綁裝置上存在的
        // com.miui.personalassistant。目標是 app 自己的類，走 app classloader。
        // 失敗只記 log，不影響已裝好的 prop hook。
        installMinusScreenHook(env, application, loader, lsplant_hook,
                               lsplant_is_hooked);

        // 第三個 hook（同進程、同 lsplant）：「小工具」按鈕改導
        // personalassistant 的小部件中心（EU 無 globalminusscreen 時
        // isMIUIWidgetSupport 恆 false，原生只會退回內建清單頁）。
        // 失敗只記 log，不影響前兩個已裝好的 hook。
        installWidgetPickerHook(env, application, loader, lsplant_hook,
                                lsplant_is_hooked);
    } while (false);

    if (hooker) env->DeleteLocalRef(hooker);
    if (target_method) env->DeleteLocalRef(target_method);
    if (hooker_cls) env->DeleteLocalRef(hooker_cls);
    if (loader) env->DeleteLocalRef(loader);
    if (application) env->DeleteLocalRef(application);
    vm->DetachCurrentThread();

    free(g_lsplant_bytes);  // bytes 已複製進 memfd；失敗路徑也一併釋放
    g_lsplant_bytes = nullptr;
    g_lsplant_size = 0;
    // lsplant_init 只要嘗試過，即使回傳失敗，先前階段裝好的 ART hook 仍然存活，
    // 其替代程式碼就在 liblsplant.so 內；此時 dlclose 會讓進程在下次觸發
    // hook 時 SIGSEGV（已在 voicetrigger tombstone 觀察到）。只有從未呼叫過
    // init（例如 dlsym 缺符號）才安全卸載。
    if (!installed && !lsplant_init_called && g_lsplant_handle) {
        dlclose(g_lsplant_handle);
        g_lsplant_handle = nullptr;
    }
    return nullptr;
}

}  // namespace

void homefeedPreloadLsplant() {
    char lsplant_path[kObfLsplantPathLen + 1];
    obf::decodeStr(kObfLsplantPath, lsplant_path);
    FILE *f = fopen(lsplant_path, "rb");
    if (!f) {
        LOGW("preload: cannot open %s: %s", lsplant_path, strerror(errno));
        obf::secureClear(lsplant_path);
        return;
    }
    obf::secureClear(lsplant_path);
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 64 * 1024 * 1024) {
        fclose(f);
        return;
    }
    auto *buf = static_cast<uint8_t *>(malloc(static_cast<size_t>(size)));
    if (!buf) {
        fclose(f);
        return;
    }
    if (fread(buf, 1, static_cast<size_t>(size), f) != static_cast<size_t>(size)) {
        free(buf);
        fclose(f);
        return;
    }
    fclose(f);
    g_lsplant_bytes = buf;
    g_lsplant_size = static_cast<size_t>(size);
}

void homefeedStartMiuiHome(JavaVM *vm) {
    if (!vm) return;
    pthread_t thread;
    if (pthread_create(&thread, nullptr, miuiHomeWorker, vm) == 0) {
        pthread_detach(thread);
    } else {
        LOGW("MiuiHome: unable to start worker");
    }
}
