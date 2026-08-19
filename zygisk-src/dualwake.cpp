#include "dualwake.h"

#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#include <cstdio>
#include <cerrno>
#include <cstring>
#include <string>

#include <sys/mman.h>
#include <sys/syscall.h>

#include "art_resolver.h"
#include "dobby.h"
#include "gen/hooker_dex.h"
#include "lsplant.hpp"

#define LOG_TAG "DualWake"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace {

// 模組 id（module.prop）決定的安裝路徑；liblsplant.so 由 build.sh 打包進這裡。
constexpr const char *kLsplantPath =
        "/data/adb/modules/HyperOS3EUXiaoAiPortalMiPay/zygisk/liblsplant.so";

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

// ---------------------------------------------------------------------------
// 共用 JNI 工具
// ---------------------------------------------------------------------------

// 等 ActivityThread.currentApplication() 可用；回傳 local ref（呼叫端負責刪）。
// settle_us：找到 Application 後額外等待的微秒數（ThemeManager 需要讓
// onCreate 先跑；CoreAlive 則必須搶在 MIUI 回收进程前，傳 0）。
jobject waitForApplication(JNIEnv *env, long settle_us) {
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
    if (application && settle_us > 0) {
        usleep(settle_us);
    }
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

// ---------------------------------------------------------------------------
// CoreAlive（com.miui.voiceassist:voice_trigger）
// ---------------------------------------------------------------------------

void *coreAliveWorker(void *opaque) {
    auto *vm = static_cast<JavaVM *>(opaque);
    JNIEnv *env = nullptr;
    if (!vm || vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
        LOGW("CoreAlive: unable to attach worker");
        return nullptr;
    }

    jobject application = waitForApplication(env, 0);
    if (!application) {
        LOGW("CoreAlive: application unavailable, skip");
        vm->DetachCurrentThread();
        return nullptr;
    }

    jobject loader = makeDexLoader(env, application);
    jclass bridge = loader ? loadClassWith(env, loader, "jrc.dualwake.CoreAliveBridge")
                           : nullptr;
    jmethodID register_on_main = bridge
            ? env->GetStaticMethodID(bridge, "registerOnMain", "(Landroid/content/Context;)V")
            : nullptr;
    if (clearException(env, "CoreAliveBridge.registerOnMain")) register_on_main = nullptr;

    if (bridge && register_on_main) {
        env->CallStaticVoidMethod(bridge, register_on_main, application);
        if (!clearException(env, "CoreAliveBridge call")) {
            LOGI("CoreAlive bridge invoked");
        }
    } else {
        LOGW("CoreAlive: bridge unavailable, safe no-op");
    }

    if (bridge) env->DeleteLocalRef(bridge);
    if (loader) env->DeleteLocalRef(loader);
    env->DeleteLocalRef(application);
    vm->DetachCurrentThread();
    return nullptr;
}

// ---------------------------------------------------------------------------
// VoiceTrigger restart hook（com.miui.voicetrigger）
// ---------------------------------------------------------------------------

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

void *voiceTriggerWorker(void *opaque) {
    auto *vm = static_cast<JavaVM *>(opaque);
    JNIEnv *env = nullptr;
    if (!vm || vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
        LOGW("VoiceTrigger: unable to attach worker");
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
        application = waitForApplication(env, 250000);
        if (!application) {
            LOGW("VoiceTrigger: application unavailable, skip");
            break;
        }

        loader = makeDexLoader(env, application);
        hooker_cls = loader
                ? loadClassWith(env, loader, "jrc.dualwake.VoiceTriggerRestartHooker")
                : nullptr;
        if (!hooker_cls) {
            LOGW("VoiceTrigger: hooker class unavailable, safe no-op");
            break;
        }

        // 版本保護：只在 VoiceTrigger 2026051416 且 q.k 存在時才繼續。
        jmethodID should_install = env->GetStaticMethodID(
                hooker_cls, "shouldInstall", "(Landroid/content/Context;)Z");
        if (clearException(env, "shouldInstall") || !should_install) break;
        jboolean ok = env->CallStaticBooleanMethod(hooker_cls, should_install, application);
        if (clearException(env, "shouldInstall call")) { ok = JNI_FALSE; }
        if (!ok) break;  // 不吻合：安全停用

        if (!g_lsplant_bytes) {
            LOGW("VoiceTrigger: liblsplant payload missing, safe no-op");
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
            LOGW("VoiceTrigger: lsplant symbols missing");
            break;
        }

        auto *resolver = new ArtResolver();  // 跟進程同壽命，不釋放
        if (!resolver->init()) {
            LOGW("VoiceTrigger: libart symbol table unavailable");
            break;
        }

        // 此 LSPlant 版本（v6.x ABI v2）只 hook GarbageCollectCache；Android 16
        // 的 libart 已沒有這個符號（併入 DoCollection，且仍在 dynsym）。簽名同為
        // void (JitCodeCache::*)(Thread*)，hook 語意相同（cache collection 前
        // 先搬走已 hook 的方法），因此查不到時改回傳 DoCollection 位址。
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
            LOGW("VoiceTrigger: lsplant Init failed");
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

        // app classloader 重新取一次，與 shouldInstall 用的是同一個。
        jclass app_class = env->GetObjectClass(application);
        jmethodID get_class_loader = app_class
                ? env->GetMethodID(app_class, "getClassLoader", "()Ljava/lang/ClassLoader;")
                : nullptr;
        jobject app_loader = (app_class && get_class_loader)
                ? env->CallObjectMethod(application, get_class_loader) : nullptr;
        if (clearException(env, "app classloader")) app_loader = nullptr;

        jclass target_cls = app_loader
                ? loadClassWith(env, app_loader, "com.miui.voicetrigger.wakeup.q")
                : nullptr;
        jmethodID target_mid = target_cls
                ? env->GetMethodID(target_cls, "k", "(Ljava/lang/String;)I") : nullptr;
        if (clearException(env, "q.k") || !target_cls || !target_mid) {
            LOGW("VoiceTrigger: restart target missing, safe no-op");
            if (target_cls) env->DeleteLocalRef(target_cls);
            if (app_loader) env->DeleteLocalRef(app_loader);
            if (app_class) env->DeleteLocalRef(app_class);
            break;
        }
        target_method = env->ToReflectedMethod(target_cls, target_mid, JNI_FALSE);
        env->DeleteLocalRef(target_cls);
        if (app_loader) env->DeleteLocalRef(app_loader);
        if (app_class) env->DeleteLocalRef(app_class);
        if (clearException(env, "q.k reflect") || !target_method) break;

        jobject backup = lsplant_hook(env, target_method, hooker, callback_method);
        if (clearException(env, "lsplant Hook") || !backup) {
            LOGW("VoiceTrigger: lsplant Hook returned null");
            break;
        }

        jfieldID backup_field = env->GetFieldID(
                hooker_cls, "backup", "Ljava/lang/reflect/Method;");
        if (clearException(env, "backup field") || !backup_field) break;
        env->SetObjectField(hooker, backup_field, backup);
        if (clearException(env, "backup set")) break;

        const bool hooked = lsplant_is_hooked(env, target_method);
        LOGI("VoiceTrigger restart hook installed (IsHooked=%d)", hooked ? 1 : 0);
        g_hooker_ref = env->NewGlobalRef(hooker);
        installed = true;
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

void dualwakePreloadLsplant() {
    FILE *f = fopen(kLsplantPath, "rb");
    if (!f) {
        LOGW("preload: cannot open %s: %s", kLsplantPath, strerror(errno));
        return;
    }
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

void dualwakeStartCoreAlive(JavaVM *vm) {
    if (!vm) return;
    pthread_t thread;
    if (pthread_create(&thread, nullptr, coreAliveWorker, vm) == 0) {
        pthread_detach(thread);
    } else {
        LOGW("CoreAlive: unable to start worker");
    }
}

void dualwakeStartVoiceTrigger(JavaVM *vm) {
    if (!vm) return;
    pthread_t thread;
    if (pthread_create(&thread, nullptr, voiceTriggerWorker, vm) == 0) {
        pthread_detach(thread);
    } else {
        LOGW("VoiceTrigger: unable to start worker");
    }
}
