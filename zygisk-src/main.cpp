// HyperOS 3 EU XiaoAI / Portal / Mi Pay — Taplus Zygisk hook
//
// 在每個 app 進程 postAppSpecialize 時，用 JNI 將
// miui.os.Build.IS_INTERNATIONAL_BUILD 翻轉為 false，
// 讓 miui.contentcatcher.InterceptorProxy.create(Activity) 不再提早 return null，
// 使 Taplus（傳送門）長按取詞在未排除的 app 生效。
//
// ThemeManager 進程則等 Application 建立完成，再把 App 內的 API region
// cache 設為 CN。這不修改全域 ro.miui.region，也不會在 Android 建立
// Application 資源時提早切區，避免 my_backup_rules / network config XML 錯配。
//
// 安全約束：
// - 所有 JNI 呼叫檢查例外並 ExceptionClear，絕不讓例外逸出到 app
// - 類/欄位不存在（非對應 ThemeManager 版本）時靜默跳過
// - 不修改任何 prop、不碰 /system*

#include <jni.h>
#include <android/log.h>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

#include "zygisk.hpp"

#define LOG_TAG "TaplusIntlFix"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace {

// 可選排除清單：一行一個 package name（nice_name），# 開頭為註解
constexpr const char *kExcludeFile =
        "/data/adb/modules/HyperOS3EUXiaoAiPortalMiPay/excluded_packages.txt";

static bool clearException(JNIEnv *env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

static bool setStaticRegionField(JNIEnv *env, jobject class_loader,
                                 jmethodID load_class, const char *class_name,
                                 const char *field_name) {
    jstring name = env->NewStringUTF(class_name);
    const bool name_exception = clearException(env);
    if (!name || name_exception) return false;

    jobject class_object = env->CallObjectMethod(class_loader, load_class, name);
    env->DeleteLocalRef(name);
    const bool class_exception = clearException(env);
    if (!class_object || class_exception) return false;

    jfieldID field = env->GetStaticFieldID(
            static_cast<jclass>(class_object), field_name, "Ljava/lang/String;");
    const bool field_exception = clearException(env);
    if (!field || field_exception) {
        env->DeleteLocalRef(class_object);
        return false;
    }

    jstring cn = env->NewStringUTF("CN");
    const bool value_exception = clearException(env);
    if (!cn || value_exception) {
        env->DeleteLocalRef(class_object);
        return false;
    }

    env->SetStaticObjectField(static_cast<jclass>(class_object), field, cn);
    const bool ok = !clearException(env);
    env->DeleteLocalRef(cn);
    env->DeleteLocalRef(class_object);
    return ok;
}

static void *themeRegionWorker(void *opaque) {
    auto *vm = static_cast<JavaVM *>(opaque);
    JNIEnv *env = nullptr;
    if (!vm || vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
        LOGW("ThemeManager: unable to attach region worker");
        return nullptr;
    }

    jclass activity_thread = env->FindClass("android/app/ActivityThread");
    const bool activity_thread_exception = clearException(env);
    jmethodID current_application = activity_thread
            ? env->GetStaticMethodID(activity_thread, "currentApplication",
                                     "()Landroid/app/Application;")
            : nullptr;
    const bool current_application_exception = clearException(env);
    if (!activity_thread || activity_thread_exception || !current_application ||
        current_application_exception) {
        if (activity_thread) env->DeleteLocalRef(activity_thread);
        vm->DetachCurrentThread();
        LOGW("ThemeManager: ActivityThread lookup failed");
        return nullptr;
    }

    bool applied = false;
    for (int attempt = 0; attempt < 500 && !applied; ++attempt) {
        jobject application = env->CallStaticObjectMethod(
                activity_thread, current_application);
        if (clearException(env)) application = nullptr;

        if (!application) {
            usleep(20000);
            continue;
        }

        // mInitialApplication is assigned immediately before Application.onCreate.
        // Give onCreate a short head start; overwriting the App caches afterwards is
        // safe even if an initial request already cached TW.
        usleep(250000);

        jclass application_class = env->GetObjectClass(application);
        jmethodID get_class_loader = application_class
                ? env->GetMethodID(application_class, "getClassLoader",
                                   "()Ljava/lang/ClassLoader;")
                : nullptr;
        jobject class_loader = get_class_loader
                ? env->CallObjectMethod(application, get_class_loader)
                : nullptr;
        if (clearException(env)) class_loader = nullptr;

        jclass class_loader_class = env->FindClass("java/lang/ClassLoader");
        jmethodID load_class = class_loader_class
                ? env->GetMethodID(class_loader_class, "loadClass",
                                   "(Ljava/lang/String;)Ljava/lang/Class;")
                : nullptr;
        if (clearException(env)) load_class = nullptr;

        if (class_loader && load_class) {
            const bool retrofit_region = setStaticRegionField(
                    env, class_loader, load_class,
                    "com.android.thememanager.basemodule.utils.ld6", "ld6");
            applied = retrofit_region;
            if (applied) {
                LOGI("ThemeManager API region cache -> CN");
            }
        }

        if (class_loader_class) env->DeleteLocalRef(class_loader_class);
        if (class_loader) env->DeleteLocalRef(class_loader);
        if (application_class) env->DeleteLocalRef(application_class);
        env->DeleteLocalRef(application);

        if (!applied) usleep(100000);
    }

    env->DeleteLocalRef(activity_thread);
    vm->DetachCurrentThread();
    if (!applied) LOGW("ThemeManager: API region cache unavailable");
    return nullptr;
}

class TaplusIntlFixModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        api_ = api;
        env_ = env;
        if (env_->GetJavaVM(&vm_) != JNI_OK) vm_ = nullptr;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        skip_ = false;
        theme_manager_ = false;
        if (!args || !args->nice_name) return;

        const char *nice = env_->GetStringUTFChars(args->nice_name, nullptr);
        if (!nice) {
            clearException(env_);
            return;
        }
        theme_manager_ = strcmp(nice, "com.android.thememanager") == 0;
        skip_ = isExcluded(nice);
        const bool sensitive = isSensitiveProcess(nice);
        if (sensitive) {
            // Payment / integrity processes should see neither systemless mounts
            // nor this module's mapped library.
            skip_ = true;
            api_->setOption(zygisk::FORCE_DENYLIST_UNMOUNT);
        }
        if (skip_) {
            LOGI("%s: excluded, unload module%s", nice,
                 sensitive ? " and force denylist unmount" : "");
            api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
        }
        env_->ReleaseStringUTFChars(args->nice_name, nice);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        if (theme_manager_) startThemeRegionWorker();
        if (!skip_) flipInternational();
    }

private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    JavaVM *vm_ = nullptr;
    bool skip_ = false;
    bool theme_manager_ = false;

    static bool matchesPackageProcess(const char *nice_name,
                                      const char *package_name) {
        const size_t length = strlen(package_name);
        return strncmp(nice_name, package_name, length) == 0 &&
               (nice_name[length] == '\0' || nice_name[length] == ':');
    }

    static bool isSensitiveProcess(const char *nice_name) {
        return matchesPackageProcess(nice_name,
                                     "com.google.android.apps.walletnfcrel") ||
               matchesPackageProcess(nice_name, "com.google.android.gms");
    }

    static bool isExcluded(const char *nice_name) {
        FILE *f = fopen(kExcludeFile, "r");
        if (!f) return false;  // 無清單檔：全部翻轉
        char line[256];
        bool hit = false;
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] == '\0' || line[0] == '#') continue;
            if (matchesPackageProcess(nice_name, line)) {
                hit = true;
                break;
            }
        }
        fclose(f);
        return hit;
    }

    void startThemeRegionWorker() {
        if (!vm_) {
            LOGW("ThemeManager: JavaVM unavailable");
            return;
        }

        pthread_t thread;
        if (pthread_create(&thread, nullptr, themeRegionWorker, vm_) == 0) {
            pthread_detach(thread);
        } else {
            LOGW("ThemeManager: unable to start region worker");
        }
    }

    void flipInternational() {
        JNIEnv *env = env_;

        jclass cls = env->FindClass("miui/os/Build");  // BOOTCLASSPATH 類，可直接解析
        if (!cls) {
            clearException(env);  // 非 MIUI：安全跳過
            return;
        }

        jfieldID fid = env->GetStaticFieldID(cls, "IS_INTERNATIONAL_BUILD", "Z");
        if (!fid) {
            clearException(env);
            env->DeleteLocalRef(cls);
            return;
        }

        if (env->GetStaticBooleanField(cls, fid)) {
            env->SetStaticBooleanField(cls, fid, JNI_FALSE);  // static final 經 JNI 可寫
            if (!env->ExceptionCheck()) LOGI("IS_INTERNATIONAL_BUILD -> false");
        }
        env->DeleteLocalRef(cls);
        clearException(env);  // 例外絕不逸出到 app
    }
};

}  // namespace

REGISTER_ZYGISK_MODULE(TaplusIntlFixModule)
