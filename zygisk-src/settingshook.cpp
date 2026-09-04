#include "settingshook.h"

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

// 本檔的 JNI / lsplant 輔助函式比照 mmedit.cpp 的實作；homefeed 有
// tombstone 級的 dlclose 教訓且正在服役，為最小侵入不抽出共用、各留一份。

// 比照 main.cpp：log 字串在 release 版編譯期移除（-DTAPLUS_DEBUG_LOG 才保留）。
#ifdef TAPLUS_DEBUG_LOG
#define LOG_TAG "SettingsHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) do {} while (0)
#define LOGW(...) do {} while (0)
#endif

namespace {

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

bool clearException(JNIEnv *env, const char *what) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    LOGW("%s: JNI exception cleared", what);
    return true;
}

// 等 ActivityThread.currentApplication() 可用；回傳 local ref（呼叫端負責刪）。
// settle 0：憑證頁只在使用者開啟時才查 provider 清單，無開機競態，仍比照
// mmedit 盡早裝好 hook。
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

// app 自身的 ClassLoader（憑證頁的目標類在 Settings APK 內，非 BOOTCLASSPATH，
// 不能 FindClass）。
jobject getAppClassLoader(JNIEnv *env, jobject application) {
    jclass app_class = env->GetObjectClass(application);
    jmethodID get_class_loader = app_class
            ? env->GetMethodID(app_class, "getClassLoader", "()Ljava/lang/ClassLoader;")
            : nullptr;
    jobject app_loader = (app_class && get_class_loader)
            ? env->CallObjectMethod(application, get_class_loader) : nullptr;
    if (clearException(env, "getClassLoader")) app_loader = nullptr;
    if (app_class) env->DeleteLocalRef(app_class);
    return app_loader;
}

// 用內嵌 dex 建 InMemoryDexClassLoader（parent = app classloader）。
jobject makeDexLoader(JNIEnv *env, jobject application, jobject app_loader) {
    jclass imcl = env->FindClass("dalvik/system/InMemoryDexClassLoader");
    if (clearException(env, "InMemoryDexClassLoader") || !imcl) return nullptr;
    jmethodID ctor = env->GetMethodID(imcl, "<init>",
            "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    if (clearException(env, "InMemoryDexClassLoader.<init>") || !ctor) {
        env->DeleteLocalRef(imcl);
        return nullptr;
    }

    jobject buffer = env->NewDirectByteBuffer(
            const_cast<unsigned char *>(hooker_dex), hooker_dex_len);
    jobject loader = (buffer && app_loader)
            ? env->NewObject(imcl, ctor, buffer, app_loader) : nullptr;
    if (clearException(env, "new InMemoryDexClassLoader")) loader = nullptr;

    if (buffer) env->DeleteLocalRef(buffer);
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

void *settingsWorker(void *opaque) {
    auto *vm = static_cast<JavaVM *>(opaque);
    JNIEnv *env = nullptr;
    if (!vm || vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
        LOGW("Settings: unable to attach worker");
        return nullptr;
    }

    bool installed = false;
    bool lsplant_init_called = false;
    jobject application = nullptr;
    jobject app_loader = nullptr;
    jobject loader = nullptr;
    jclass hooker_cls = nullptr;
    jobject hooker = nullptr;
    jclass target_cls = nullptr;
    jobject target_method = nullptr;

    // hook 目標的類名／方法名／簽名由 gen_obf_strings.py 編碼（app 內部類屬
    // hook target class name，不明文進 .so），用時解到 stack、用完抹除。
    char target_class[kObfCredListClassLen + 1],
         target_name[kObfCredListMethodLen + 1],
         target_sig[kObfCredListSigLen + 1];
    obf::decodeStr(kObfCredListClass, target_class);
    obf::decodeStr(kObfCredListMethod, target_name);
    obf::decodeStr(kObfCredListSig, target_sig);

    do {
        application = waitForApplication(env);
        if (!application) {
            LOGW("Settings: application unavailable, skip");
            break;
        }

        app_loader = getAppClassLoader(env, application);
        loader = app_loader ? makeDexLoader(env, application, app_loader) : nullptr;
        hooker_cls = loader
                ? loadClassWith(env, loader, "jrc.settings.CredListHooker")
                : nullptr;
        if (!hooker_cls) {
            LOGW("Settings: hooker class unavailable, safe no-op");
            break;
        }

        // 目標 shape 保護：驗證目標方法與反射用 full-list 方法都在（不 pin
        // app 版本，理由見 CredListHooker 類註）。
        jmethodID should_install = env->GetStaticMethodID(
                hooker_cls, "shouldInstall", "()Z");
        if (clearException(env, "shouldInstall") || !should_install) break;
        jboolean ok = env->CallStaticBooleanMethod(hooker_cls, should_install);
        if (clearException(env, "shouldInstall call")) { ok = JNI_FALSE; }
        if (!ok) break;  // 不吻合：安全停用

        if (!g_lsplant_bytes) {
            LOGW("Settings: liblsplant payload missing, safe no-op");
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
            LOGW("Settings: lsplant symbols missing");
            break;
        }

        auto *resolver = new ArtResolver();  // 跟進程同壽命，不釋放
        if (!resolver->init()) {
            LOGW("Settings: libart symbol table unavailable");
            break;
        }

        // GarbageCollectCache → DoCollection 的 Android 16 回退理由同
        // dualwake.cpp；各檔若有一邊要改，其他邊也要改。
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
            LOGW("Settings: lsplant Init failed");
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

        // 目標是 Settings APK 內的類，經 app classloader 解析（非
        // BOOTCLASSPATH，不能 FindClass）。
        target_cls = loadClassWith(env, app_loader, target_class);
        jmethodID target_mid = target_cls
                ? env->GetStaticMethodID(target_cls, target_name, target_sig)
                : nullptr;
        if (clearException(env, "getCombinedProviderInfos") || !target_cls || !target_mid) {
            LOGW("Settings: credman target missing, safe no-op");
            break;
        }
        target_method = env->ToReflectedMethod(target_cls, target_mid, JNI_TRUE);
        if (clearException(env, "getCombinedProviderInfos reflect") || !target_method) break;

        jobject backup = lsplant_hook(env, target_method, hooker, callback_method);
        if (clearException(env, "lsplant Hook") || !backup) {
            LOGW("Settings: lsplant Hook returned null");
            break;
        }

        jfieldID backup_field = env->GetFieldID(
                hooker_cls, "backup", "Ljava/lang/reflect/Method;");
        if (clearException(env, "backup field") || !backup_field) break;
        env->SetObjectField(hooker, backup_field, backup);
        if (clearException(env, "backup set")) break;

        const bool hooked = lsplant_is_hooked(env, target_method);
        LOGI("Settings credman list hook installed (IsHooked=%d)", hooked ? 1 : 0);
        g_hooker_ref = env->NewGlobalRef(hooker);
        installed = true;
    } while (false);

    obf::secureClear(target_class);
    obf::secureClear(target_name);
    obf::secureClear(target_sig);

    if (hooker) env->DeleteLocalRef(hooker);
    if (target_method) env->DeleteLocalRef(target_method);
    if (target_cls) env->DeleteLocalRef(target_cls);
    if (hooker_cls) env->DeleteLocalRef(hooker_cls);
    if (loader) env->DeleteLocalRef(loader);
    if (app_loader) env->DeleteLocalRef(app_loader);
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

void settingshookPreloadLsplant() {
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

void settingshookStartSettings(JavaVM *vm) {
    if (!vm) return;
    pthread_t thread;
    if (pthread_create(&thread, nullptr, settingsWorker, vm) == 0) {
        pthread_detach(thread);
    } else {
        LOGW("Settings: unable to start worker");
    }
}
