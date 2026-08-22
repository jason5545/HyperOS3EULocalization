// HyperOS 3 EU XiaoAI / Portal / Mi Pay — Taplus Zygisk hook
//
// 在每個 app 進程 postAppSpecialize 時，用 JNI 將
// miui.os.Build.IS_INTERNATIONAL_BUILD 翻轉為 false，
// 讓 miui.contentcatcher.InterceptorProxy.create(Activity) 不再提早 return null，
// 使 Taplus（傳送門）長按取詞在未排除的 app 生效。
//
// ThemeManager 進程則在 Application 物件一出現時（onCreate 完成前），就把
// App 內的 API region lazy cache（DeviceUtils.ld6）設為 CN —— 該欄位
// write-once，先寫先贏，必須趕在首個 API 請求把它快取成真實 region 之前。
// 這不修改全域 ro.miui.region，也不影響 Android 建立 Application 資源時的
// 區域判斷，避免 my_backup_rules / network config XML 錯配。
//
// 安全約束：
// - 所有 JNI 呼叫檢查例外並 ExceptionClear，絕不讓例外逸出到 app
// - 類/欄位不存在（非對應 ThemeManager 版本）時靜默跳過
// - 不修改任何 prop、不碰 /system*
// - 預設無任何 logcat 輸出；敏感進程連除錯旗標都不讀，永遠靜默
// - Wallet / Play Store / GMS（含 .unstable，DroidGuard 所在）進程：
//   最先處理，立即 force denylist unmount + dlclose，不做任何修改

#include <jni.h>
#include <android/log.h>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

#include "dualwake.h"
#include "zygisk.hpp"

#define LOG_TAG "TaplusIntlFix"
// 預設完全靜默：logcat 不該留下任何模組足跡。
// 除錯時建立空檔 /data/adb/modules/HyperOS3EUXiaoAiPortalMiPay/debug 才輸出，
// 且只對非敏感進程生效（敏感進程永不讀檔、永不輸出）。
#define LOGI(...) do { if (g_debug) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__); } while (0)
#define LOGW(...) do { if (g_debug) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__); } while (0)

namespace {

// 可選排除清單：一行一個 package name（nice_name），# 開頭為註解
constexpr const char *kExcludeFile =
        "/data/adb/modules/HyperOS3EUXiaoAiPortalMiPay/excluded_packages.txt";

// 除錯旗標：存在即開啟 log。僅在非敏感進程的 preAppSpecialize 讀取一次
// （當下仍是 zygote 權限，讀 /data/adb 不會留下 app 側的存取痕跡）。
constexpr const char *kDebugFile =
        "/data/adb/modules/HyperOS3EUXiaoAiPortalMiPay/debug";
static bool g_debug = false;

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

        // mInitialApplication 在 Application.onCreate 之前就被賦值，此時連
        // 首個 Activity 都還沒建立。立刻翻轉，不給 head start：
        // DeviceUtils.ld6 是 write-once lazy cache（i() 只在欄位為 null 時
        // 才填入 miui.os.Build.getRegion()，ParamInterceptor 每個 API 請求
        // 都讀它），先寫者贏。冷啟動首頁空白的根因正是舊版在此睡了 250ms，
        // 讓首頁請求搶先把真實 region 快取進去。

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
            // 10.8.0.0: basemodule.utils.ld6；10.8.7.6 起類名改回 DeviceUtils，
            // 區域快取欄位都叫 ld6。依序嘗試，命中任一即算成功。
            const bool retrofit_region = setStaticRegionField(
                    env, class_loader, load_class,
                    "com.android.thememanager.basemodule.utils.DeviceUtils", "ld6") ||
                setStaticRegionField(
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
        core_alive_ = false;
        voice_trigger_ = false;
        if (!args || !args->nice_name) return;

        const char *nice = env_->GetStringUTFChars(args->nice_name, nullptr);
        if (!nice) {
            clearException(env_);
            return;
        }
        // 支付／完整性檢查進程第一優先處理：不做任何檔案 I/O、不輸出任何
        // log、不翻 Build 欄位，立刻要求系統卸載所有模組掛載並 dlclose
        // 本模組 library，讓這類進程從 specialization 結束起就完全看不見本模組。
        if (isSensitiveProcess(nice)) {
            skip_ = true;
            api_->setOption(zygisk::FORCE_DENYLIST_UNMOUNT);
            api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            env_->ReleaseStringUTFChars(args->nice_name, nice);
            return;
        }

        // 到這裡已確定不是敏感進程；仍是 zygote 權限，安全地讀一次除錯旗標。
        g_debug = access(kDebugFile, F_OK) == 0;

        theme_manager_ = strcmp(nice, "com.android.thememanager") == 0;
        // 雙喚醒目標進程：精確辨識，絕不影響其他 app。
        core_alive_ = strcmp(nice, "com.miui.voiceassist:voice_trigger") == 0;
        voice_trigger_ = strcmp(nice, "com.miui.voicetrigger") == 0 ||
                         strncmp(nice, "com.miui.voicetrigger:",
                                 strlen("com.miui.voicetrigger:")) == 0;
        skip_ = isExcluded(nice);
        if (core_alive_ || voice_trigger_) {
            // 雙喚醒 worker 需要模組常駐；即使排除清單誤加這兩個 package，
            // 也不能 DLCLOSE 自己。
            skip_ = false;
        }
        if (skip_) {
            LOGI("%s: excluded, unload module", nice);
            api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
        }
        if (voice_trigger_) {
            // 仍是 zygote 權限：先讀好 liblsplant.so，post 階段只需 memfd。
            dualwakePreloadLsplant();
        }
        env_->ReleaseStringUTFChars(args->nice_name, nice);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        if (core_alive_) dualwakeStartCoreAlive(vm_);
        if (voice_trigger_) dualwakeStartVoiceTrigger(vm_);
        if (theme_manager_) startThemeRegionWorker();
        if (!skip_) flipInternational();
    }

private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    JavaVM *vm_ = nullptr;
    bool skip_ = false;
    bool theme_manager_ = false;
    bool core_alive_ = false;
    bool voice_trigger_ = false;

    static bool matchesPackageProcess(const char *nice_name,
                                      const char *package_name) {
        // 子進程有兩種命名："pkg:remote"（冒號）與 "pkg.remote"（完整點號名，
        // 如 SecurityCenter 的 com.miui.securitycenter.remote，NetworkAssistant
        // provider 所在）。兩者都算同一 package，否則點號子進程會漏出排除清單。
        const size_t length = strlen(package_name);
        return strncmp(nice_name, package_name, length) == 0 &&
               (nice_name[length] == '\0' || nice_name[length] == ':' ||
                nice_name[length] == '.');
    }

    static bool isSensitiveProcess(const char *nice_name) {
        // gms 除了主進程與 :子進程，還有用「.」命名的獨立 process，
        // 最重要的是 com.google.android.gms.unstable（DroidGuard /
        // Play Integrity 實際執行處）；三者都必須涵蓋。
        constexpr const char *kGms = "com.google.android.gms";
        const size_t gms_length = strlen(kGms);
        if (strncmp(nice_name, kGms, gms_length) == 0) {
            const char tail = nice_name[gms_length];
            if (tail == '\0' || tail == ':' || tail == '.') return true;
        }
        return matchesPackageProcess(nice_name,
                                     "com.google.android.apps.walletnfcrel") ||
               matchesPackageProcess(nice_name, "com.android.vending");
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
