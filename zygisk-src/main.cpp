// HyperOS 3 EU XiaoAI / Portal / Mi Pay — Taplus Zygisk hook
//
// 在每個 app 進程 postAppSpecialize 時，用 JNI 將
// miui.os.Build.IS_INTERNATIONAL_BUILD 翻轉為 false，
// 讓 miui.contentcatcher.InterceptorProxy.create(Activity) 不再提早 return null，
// 使 Taplus（傳送門）長按取詞在所有 app 生效。
//
// 安全約束：
// - 所有 JNI 呼叫檢查例外並 ExceptionClear，絕不讓例外逸出到 app
// - 類/欄位不存在（非 MIUI）時靜默跳過
// - 不修改任何 prop、不碰 /system*

#include <jni.h>
#include <android/log.h>
#include <cstdio>
#include <cstring>

#include "zygisk.hpp"

#define LOG_TAG "TaplusIntlFix"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace {

// 可選排除清單：一行一個 package name（nice_name），# 開頭為註解
constexpr const char *kExcludeFile =
        "/data/adb/modules/HyperOS3EUXiaoAiPortalMiPay/excluded_packages.txt";

class TaplusIntlFixModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        api_ = api;
        env_ = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        skip_ = false;
        if (!args || !args->nice_name) return;

        const char *nice = env_->GetStringUTFChars(args->nice_name, nullptr);
        if (!nice) {
            if (env_->ExceptionCheck()) env_->ExceptionClear();
            return;
        }
        skip_ = isExcluded(nice);
        if (skip_) LOGI("%s: excluded, skip", nice);
        env_->ReleaseStringUTFChars(args->nice_name, nice);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        if (skip_) return;
        flipInternational();
    }

private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    bool skip_ = false;

    static bool isExcluded(const char *nice_name) {
        FILE *f = fopen(kExcludeFile, "r");
        if (!f) return false;  // 無清單檔：全部翻轉
        char line[256];
        bool hit = false;
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] == '\0' || line[0] == '#') continue;
            if (strcmp(line, nice_name) == 0) { hit = true; break; }
        }
        fclose(f);
        return hit;
    }

    void flipInternational() {
        JNIEnv *env = env_;

        jclass cls = env->FindClass("miui/os/Build");  // BOOTCLASSPATH 類，可直接解析
        if (!cls) {
            env->ExceptionClear();  // 非 MIUI：安全跳過
            return;
        }

        jfieldID fid = env->GetStaticFieldID(cls, "IS_INTERNATIONAL_BUILD", "Z");
        if (!fid) {
            env->ExceptionClear();
            env->DeleteLocalRef(cls);
            return;
        }

        if (env->GetStaticBooleanField(cls, fid)) {
            env->SetStaticBooleanField(cls, fid, JNI_FALSE);  // static final 經 JNI 可寫
            if (!env->ExceptionCheck()) LOGI("IS_INTERNATIONAL_BUILD -> false");
        }
        env->DeleteLocalRef(cls);

        if (env->ExceptionCheck()) env->ExceptionClear();  // 例外絕不逸出到 app
    }
};

}  // namespace

REGISTER_ZYGISK_MODULE(TaplusIntlFixModule)
