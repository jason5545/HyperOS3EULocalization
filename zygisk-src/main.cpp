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
// - 預設無任何 logcat 輸出：release 版連 log 字串都在編譯期移除
//   （-DTAPLUS_DEBUG_LOG 才保留）；敏感／金融進程連除錯旗標都不讀
// - 路徑、package 名、hook 目標類名等常數編譯期 XOR 編碼（obfstr.h），
//   只在使用當下解到 stack、用完抹除，不讓明文長駐可讀記憶體
// - Wallet / Play Store / GMS（含 .unstable，DroidGuard 所在）進程：
//   最先處理，立即 force denylist unmount + dlclose，不做任何修改
// - 金融／支付 app（台灣主要銀行前綴 + 街口／支付寶等）：同樣第一優先、
//   不讀任何檔、不輸出 log、永不翻轉、立即 dlclose——但「不」force denylist
//   unmount。這類 app 的 RASP 偵測的正是「KSU 對本 app 設定 per-app umount」
//   這個狀態本身（mount namespace 差異），主動 unmount 反而製造它要抓的
//   證據；umount 策略統一交給 KSU 全域（susfs）層，本模組只負責從該進程消失
// - com.miui.home 桌面：永不翻轉 IS_INTERNATIONAL_BUILD（CN 版桌面的
//   Google 負一屏分支以它為第一道門），也永不 dlclose——homefeed worker
//   需要在該進程常駐，攔截 ro.com.miui.rsa 的讀取讓 Google Feed 可選
// - com.miui.mediaeditor 相簿編輯器：永不 dlclose——mmedit worker 常駐，
//   攔截 ro.miui.region 的讀取改回 "CN"，讓 AigcCloud 的雲控 region→URL
//   表選到 CN 推論端點（intl SGP 端點對本帳號的超高畫質任務恆回
//   taskStatus 4003，2026-08-28 myron 實測 CN 端點正常）。Taplus 翻轉
//   照舊（它不是 miui_home）。
// - com.android.settings 系統設定：永不 dlclose——settingshook worker 常駐，
//   把憑證頁 DefaultCombinedPreferenceController.getCombinedProviderInfos
//   換成其 INTL 分支（完整 credential provider 清單），只解除該頁的 CN
//   過濾；Taplus 翻轉對 Settings 其餘部分照舊生效（它不是 miui_home）。

#include <jni.h>
#include <android/log.h>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

#include "dualwake.h"
#include "homefeed.h"
#include "mmedit.h"
#include "settingshook.h"
#include "obfstr.h"
#include "gen/obf_strings.h"
#include "zygisk.hpp"

// log 字串在 release 版於編譯期整個移除（連 .rodata 都不留）——模組 .so
// 會在未排除 app 的 maps 裡被宿主直接讀取。需要 log 時以 -DTAPLUS_DEBUG_LOG
// 編譯（host mock test 即是），除錯旗標
// /data/adb/modules/HyperOS3EUXiaoAiPortalMiPay/debug 仍負責 runtime 開關，
// 且只對非敏感、非金融進程生效（敏感／金融進程永不讀檔、永不輸出）。
#ifdef TAPLUS_DEBUG_LOG
#define LOG_TAG "TaplusIntlFix"
#define LOGI(...) do { if (g_debug) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__); } while (0)
#define LOGW(...) do { if (g_debug) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__); } while (0)
#else
#define LOGI(...) do {} while (0)
#define LOGW(...) do {} while (0)
#endif

namespace {

// 可選排除清單：一行一個 package name（nice_name），# 開頭為註解。
// 除錯旗標：存在即開啟 log。僅在非敏感進程的 preAppSpecialize 讀取一次
// （當下仍是 zygote 權限，讀 /data/adb 不會留下 app 側的存取痕跡）。
// 兩條路徑常數都由 gen_obf_strings.py 編碼（gen/obf_strings.h），用時才
// 解到 stack、用完抹除（見 obfstr.h）。
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
            char device_utils[kObfDeviceUtilsLen + 1],
                 ld6_class[kObfLd6ClassLen + 1];
            obf::decodeStr(kObfDeviceUtils, device_utils);
            obf::decodeStr(kObfLd6Class, ld6_class);
            const bool retrofit_region = setStaticRegionField(
                    env, class_loader, load_class, device_utils, "ld6") ||
                setStaticRegionField(
                    env, class_loader, load_class, ld6_class, "ld6");
            obf::secureClear(device_utils);
            obf::secureClear(ld6_class);
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
        miui_home_ = false;
        media_editor_ = false;
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
        // 金融偵測型進程：與敏感進程同樣第一優先——不讀檔、不 log、不翻轉，
        // 但只 dlclose 自己，不 FORCE_DENYLIST_UNMOUNT（理由見檔頭註解）。
        if (isFinancialProcess(nice)) {
            skip_ = true;
            api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            env_->ReleaseStringUTFChars(args->nice_name, nice);
            return;
        }

        // 到這裡已確定不是敏感進程；仍是 zygote 權限，安全地讀一次除錯旗標。
        {
            char debug_file[kObfDebugFileLen + 1];
            obf::decodeStr(kObfDebugFile, debug_file);
            g_debug = access(debug_file, F_OK) == 0;
            obf::secureClear(debug_file);
        }

        {
            char theme_manager[kObfThemeManagerLen + 1],
                 core_alive[kObfCoreAliveLen + 1],
                 voice_trigger[kObfVoiceTriggerLen + 1],
                 voice_trigger_pfx[kObfVoiceTriggerPfxLen + 1],
                 miui_home[kObfMiuiHomeLen + 1],
                 media_editor[kObfMediaEditorLen + 1],
                 settings_app[kObfSettingsAppLen + 1];
            obf::decodeStr(kObfThemeManager, theme_manager);
            obf::decodeStr(kObfCoreAlive, core_alive);
            obf::decodeStr(kObfVoiceTrigger, voice_trigger);
            obf::decodeStr(kObfVoiceTriggerPfx, voice_trigger_pfx);
            obf::decodeStr(kObfMiuiHome, miui_home);
            obf::decodeStr(kObfMediaEditor, media_editor);
            obf::decodeStr(kObfSettingsApp, settings_app);

            theme_manager_ = strcmp(nice, theme_manager) == 0;
            // 雙喚醒目標進程：精確辨識，絕不影響其他 app。
            core_alive_ = strcmp(nice, core_alive) == 0;
            voice_trigger_ = strcmp(nice, voice_trigger) == 0 ||
                             strncmp(nice, voice_trigger_pfx,
                                     strlen(voice_trigger_pfx)) == 0;
            // 桌面進程：CN 版桌面的 Google 負一屏由 homefeed worker 負責，
            // 模組必須常駐，且絕不翻轉 IS_INTERNATIONAL_BUILD（見 postAppSpecialize）。
            miui_home_ = matchesPackageProcess(nice, miui_home);
            // 相簿編輯器進程：mmedit worker 的 region prop hook 需要模組常駐
            // （Taplus 翻轉照舊，見 postAppSpecialize）。
            media_editor_ = matchesPackageProcess(nice, media_editor);
            // 系統設定進程：settingshook worker 需要模組常駐（Taplus 翻轉
            // 照舊；精確匹配主進程即可，憑證頁不跑子進程）。
            miui_settings_ = strcmp(nice, settings_app) == 0;

            obf::secureClear(theme_manager);
            obf::secureClear(core_alive);
            obf::secureClear(voice_trigger);
            obf::secureClear(voice_trigger_pfx);
            obf::secureClear(miui_home);
            obf::secureClear(media_editor);
            obf::secureClear(settings_app);
        }
        skip_ = isExcluded(nice);
        if (core_alive_ || voice_trigger_ || miui_home_ || media_editor_ ||
                miui_settings_) {
            // 雙喚醒 worker、桌面 prop hook、編輯器 region hook 與設定憑證頁
            // hook 都需要模組常駐；即使排除清單誤加這些 package，也不能
            // DLCLOSE 自己。
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
        if (miui_home_) {
            homefeedPreloadLsplant();
        }
        if (media_editor_) {
            mmeditPreloadLsplant();
        }
        if (miui_settings_) {
            settingshookPreloadLsplant();
        }
        env_->ReleaseStringUTFChars(args->nice_name, nice);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        if (core_alive_) dualwakeStartCoreAlive(vm_);
        if (voice_trigger_) dualwakeStartVoiceTrigger(vm_);
        if (theme_manager_) startThemeRegionWorker();
        if (miui_home_) homefeedStartMiuiHome(vm_);
        if (media_editor_) mmeditStartEditor(vm_);
        if (miui_settings_) settingshookStartSettings(vm_);
        // 桌面永不翻轉：CN 版 LauncherAssistantCompat.newInstance 以
        // miui.os.Build.IS_INTERNATIONAL_BUILD 為 Google/global 負一屏的
        // 第一道門，翻成 false 反而讓 Google Feed 消失。
        if (!skip_ && !miui_home_) flipInternational();
    }

private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    JavaVM *vm_ = nullptr;
    bool skip_ = false;
    bool theme_manager_ = false;
    bool core_alive_ = false;
    bool voice_trigger_ = false;
    bool miui_home_ = false;
    bool media_editor_ = false;
    bool miui_settings_ = false;

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
        char gms[kObfGmsLen + 1], wallet[kObfWalletLen + 1],
             vending[kObfVendingLen + 1];
        obf::decodeStr(kObfGms, gms);
        obf::decodeStr(kObfWallet, wallet);
        obf::decodeStr(kObfVending, vending);

        const size_t gms_length = strlen(gms);
        bool sensitive = false;
        if (strncmp(nice_name, gms, gms_length) == 0) {
            const char tail = nice_name[gms_length];
            if (tail == '\0' || tail == ':' || tail == '.') sensitive = true;
        }
        if (!sensitive) {
            sensitive = matchesPackageProcess(nice_name, wallet) ||
                        matchesPackageProcess(nice_name, vending);
        }
        obf::secureClear(gms);
        obf::secureClear(wallet);
        obf::secureClear(vending);
        return sensitive;
    }

    static bool isFinancialProcess(const char *nice_name) {
        // 台灣主要銀行／支付 app 清單（一行一筆，見 gen_obf_strings.py）。
        // 來源：PrivSec-dev 銀行相容清單 Taiwan 段 + 實機 pm list packages
        // 核對（2026-08，myron）。以「.」結尾的視為前綴，只命中該機構自己的
        // package 命名空間（含其 :child 子進程）；結尾非「.」的視為完整
        // package，邊界比對同 matchesPackageProcess。誤傷的代價也只是
        // 該 app 失去 Taplus 翻轉。
        char list[kObfFinancialListLen + 1];
        obf::decodeStr(kObfFinancialList, list);
        bool hit = false;
        const char *p = list;
        while (!hit && *p) {
            const char *nl = strchr(p, '\n');
            const size_t len = nl ? (size_t)(nl - p) : strlen(p);
            if (len > 0) {
                const bool prefix = p[len - 1] == '.';
                hit = prefix
                        ? strncmp(nice_name, p, len) == 0
                        : strncmp(nice_name, p, len) == 0 &&
                          (nice_name[len] == '\0' || nice_name[len] == ':' ||
                           nice_name[len] == '.');
            }
            if (!nl) break;
            p = nl + 1;
        }
        obf::secureClear(list);
        return hit;
    }

    static bool isExcluded(const char *nice_name) {
        char exclude_file[kObfExcludeFileLen + 1];
        obf::decodeStr(kObfExcludeFile, exclude_file);
        FILE *f = fopen(exclude_file, "r");
        obf::secureClear(exclude_file);
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

        char build_class[kObfBuildClassLen + 1],
             intl_field[kObfIntlFieldLen + 1];
        obf::decodeStr(kObfBuildClass, build_class);
        obf::decodeStr(kObfIntlField, intl_field);

        jclass cls = env->FindClass(build_class);  // BOOTCLASSPATH 類，可直接解析
        obf::secureClear(build_class);
        if (!cls) {
            clearException(env);  // 非 MIUI：安全跳過
            obf::secureClear(intl_field);
            return;
        }

        jfieldID fid = env->GetStaticFieldID(cls, intl_field, "Z");
        obf::secureClear(intl_field);
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
