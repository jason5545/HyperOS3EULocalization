#!/system/bin/sh

MODDIR=${0%/*}
SYSTEM_VERSION="$(getprop ro.system.build.version.incremental)"
SYSTEM_VERSION=${SYSTEM_VERSION:-unknown}
VERSION_DIR="$MODDIR/system/etc/localization/SystemVersion"

if [ ! -f "$VERSION_DIR/$SYSTEM_VERSION" ]; then
    rm -rf /data/system/package_cache/*
    rm -rf "$VERSION_DIR"
    mkdir -p "$VERSION_DIR"
    touch "$VERSION_DIR/$SYSTEM_VERSION"
fi

# 等系統服務與 user 0 ready，再套用每個 App 的語系。
# 小愛、語音喚醒、語音引擎與 AI 通話使用簡中；其餘新增 App 使用繁中（台灣）。
BOOT_WAIT=0
while [ "$(getprop sys.boot_completed)" != "1" ] && [ "$BOOT_WAIT" -lt 120 ]; do
    sleep 2
    BOOT_WAIT=$((BOOT_WAIT + 1))
done

# 語音引擎、Gallery、MediaEditor、SoundRecorder 走正常 /data/app 安裝，讓 Android
# 自己解出 native libraries；不要建立額外 bind mount，避免支付 App 看見
# KernelSU 模組掛載。ThemeManager 因 shared UID/重複 permission 保留 systemless。
DATA_PAYLOAD_DIR="$MODDIR/payload"
DATA_INSTALL_TMP=/data/local/tmp/jrc_data_app_install
DATA_INSTALL_LOG="$MODDIR/data_app_install.log"

installed_version_code() {
    dumpsys package "$1" 2>/dev/null \
        | sed -n 's/.*versionCode=\([0-9][0-9]*\).*/\1/p' \
        | head -n 1
}

ensure_data_app() {
    PACKAGE_NAME="$1"
    RELATIVE_APK="$2"
    EXPECTED_VERSION="$3"
    APK_PATH="$DATA_PAYLOAD_DIR/$RELATIVE_APK"
    APK_NAME="${RELATIVE_APK##*/}"

    if [ "$(installed_version_code "$PACKAGE_NAME")" = "$EXPECTED_VERSION" ]; then
        return
    fi
    if [ ! -f "$APK_PATH" ]; then
        echo "MISSING: $APK_PATH" >> "$DATA_INSTALL_LOG"
        return
    fi

    mkdir -p "$DATA_INSTALL_TMP"
    TMP_APK="$DATA_INSTALL_TMP/$APK_NAME"
    cp "$APK_PATH" "$TMP_APK"
    if pm install -r -d -g "$TMP_APK" >> "$DATA_INSTALL_LOG" 2>&1; then
        echo "SUCCESS: $PACKAGE_NAME -> $EXPECTED_VERSION" >> "$DATA_INSTALL_LOG"
    else
        echo "FAILED: $PACKAGE_NAME -> $EXPECTED_VERSION" >> "$DATA_INSTALL_LOG"
    fi
    rm -f "$TMP_APK"
}

echo "=== data app ensure: $(date) ===" > "$DATA_INSTALL_LOG"
ensure_data_app com.xiaomi.mibrain.speech xiaoai/MIUIXiaoAiSpeechEngine.apk 60
ensure_data_app com.miui.gallery cn-media/MiuiGallery.apk 5000507
ensure_data_app com.miui.mediaeditor cn-media/MiMediaEditor.apk 203990083
ensure_data_app com.android.soundrecorder cn-media/SoundRecorder.apk 708093

# 官方 308 static overlay 在 hybrid_mount 環境可能無法新增到 /product/overlay；
# 改由 Package Manager 安裝並讓 OverlayManager 啟用。
THEME_OVERLAY_PACKAGE=com.android.thememanager.customthemeconfig.config.overlay
THEME_OVERLAY_APK="$DATA_PAYLOAD_DIR/cn-media/MiuiThemeManagerCnOverlay.apk"
if ! pm path "$THEME_OVERLAY_PACKAGE" >/dev/null 2>&1 && [ -f "$THEME_OVERLAY_APK" ]; then
    mkdir -p "$DATA_INSTALL_TMP"
    cp "$THEME_OVERLAY_APK" "$DATA_INSTALL_TMP/MiuiThemeManagerCnOverlay.apk"
    pm install -r -d -g "$DATA_INSTALL_TMP/MiuiThemeManagerCnOverlay.apk" \
        >> "$DATA_INSTALL_LOG" 2>&1
    rm -f "$DATA_INSTALL_TMP/MiuiThemeManagerCnOverlay.apk"
fi
cmd overlay enable --user 0 "$THEME_OVERLAY_PACKAGE" >/dev/null 2>&1
rm -rf "$DATA_INSTALL_TMP"

set_app_locale() {
    PACKAGE_NAME="$1"
    LOCALE_CONFIG="$2"
    APP_LOCALE="$3"
    cmd locale set-app-localeconfig "$PACKAGE_NAME" --user 0 --locales "$LOCALE_CONFIG" >/dev/null 2>&1
    cmd locale set-app-locales "$PACKAGE_NAME" --user 0 --locales "$APP_LOCALE" >/dev/null 2>&1
}

set_app_locale com.miui.voiceassist zh-CN,zh-TW zh-CN
set_app_locale com.miui.voicetrigger zh-CN,zh-TW zh-CN
set_app_locale com.xiaomi.mibrain.speech zh-CN,zh-TW zh-CN
set_app_locale com.xiaomi.aiasst.vision zh-CN,zh-TW zh-CN
set_app_locale com.xiaomi.aiasst.service zh-CN,zh-TW zh-CN

set_app_locale com.miui.contentextension zh-TW,zh-CN zh-TW
set_app_locale com.miui.nextpay zh-TW,zh-CN zh-TW
set_app_locale com.miui.tsmclient zh-TW,zh-CN zh-TW
set_app_locale com.unionpay.tsmservice.mi zh-TW,zh-CN zh-TW
set_app_locale com.xiaomi.payment zh-TW,zh-CN zh-TW
set_app_locale com.miui.gallery zh-TW,zh-CN zh-TW
set_app_locale com.miui.mediaeditor zh-TW,zh-CN zh-TW
set_app_locale com.android.soundrecorder zh-TW,zh-CN zh-TW
set_app_locale com.android.thememanager zh-TW,zh-CN zh-TW

# --- 雙喚醒冷開機保底 -------------------------------------------------------
# 冷開機記憶體高峰時，MIUI 可能在 BootupReceiver 結束後數十毫秒內回收
# com.miui.voiceassist:voice_trigger，讓 CoreAlive 內部的 bind 來不及執行。
# 等系統穩定後檢查 VoiceTriggerService 是否已由小愛自己綁定；沒有才對
# 小愛自己的 BootupReceiver 重送開機廣播（不直接啟動 VoiceTrigger 服務，
# 不切預設助理；綁定仍由 308 官方 CoreAlive 鏈完成，caller 是小愛）。
DUALWAKE_LOG="$MODDIR/dualwake_boot.log"
if [ "$(settings get global voice_trigger_enabled)" = "1" ]; then
    case "$(settings get secure voice_interaction_service)" in
    com.miui.voiceassist/*)
        : # 小愛已是預設助理：官方 VoiceInteractionService 鏈自己會處理
        ;;
    *)
        DUALWAKE_RETRY=0
        while [ "$DUALWAKE_RETRY" -lt 3 ]; do
            sleep 60
            if dumpsys activity services com.miui.voicetrigger 2>/dev/null \
                    | grep -q "VoiceTriggerService"; then
                break
            fi
            DUALWAKE_RETRY=$((DUALWAKE_RETRY + 1))
            echo "retry $DUALWAKE_RETRY: re-deliver BootupReceiver $(date)" \
                >> "$DUALWAKE_LOG"
            am broadcast -a android.intent.action.BOOT_COMPLETED \
                -n com.miui.voiceassist/com.xiaomi.voiceassistant.voiceTrigger.adapter.BootupReceiver \
                >> "$DUALWAKE_LOG" 2>&1
        done
        ;;
    esac
fi
