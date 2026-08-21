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

# 語音引擎、Gallery 走正常 /data/app 安裝，讓 Android
# 自己解出 native libraries；不要建立額外 bind mount，避免支付 App 看見
# KernelSU 模組掛載。ThemeManager 因 shared UID/重複 permission 保留 systemless。
# SoundRecorder 自 v1.0.11 改為 systemless priv-app：搭配模組自帶的
# privapp 授權 XML 才有 WRITE_MEDIA_STORAGE／CAPTURE_AUDIO_OUTPUT。
# MediaEditor 自 v1.0.12 改為 systemless：EU 308 底包內建 2.4.0.4.3-global
# （vc 204990043）高於 CN 版（203990083），data-app 會被當過期更新丟棄。
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

# 曾走 /data/app 的套件（SoundRecorder、MediaEditor）在同 versionCode 下會被
# data 安裝遮蔽，這裡卸掉殘留的 data 安裝，讓 systemless 版本生效。
# 錄音檔與相片都在 MediaStore，卸掉 data 安裝不影響既有檔案。
for MIGRATE_PKG in com.android.soundrecorder com.miui.mediaeditor; do
    case "$(pm path "$MIGRATE_PKG" 2>/dev/null | head -n 1)" in
        package:/data/app/*)
            if pm uninstall "$MIGRATE_PKG" >> "$DATA_INSTALL_LOG" 2>&1; then
                echo "MIGRATED: $MIGRATE_PKG data -> module" >> "$DATA_INSTALL_LOG"
            fi
            ;;
    esac
done

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

app_locale_is() {
    [ "$(cmd locale get-app-locales "$1" --user 0 2>/dev/null \
        | sed -n 's/.*are \[\(.*\)\]/\1/p')" = "$2" ]
}

ensure_app_locale() {
    app_locale_is "$1" "$3" && return
    LOCALE_FAILED=1
    set_app_locale "$1" "$2" "$3"
}

# /data/system_ce 要解鎖後才掛載，boot_completed 可能早於解鎖：直接寫會吃
# LocaleManagerService IOException，該次開機的語系設定整個沒生效
# （2026-08-21 07:46 實測，開機後 30 秒內前兩個 App 寫入失敗）。
# 先等 ce ready（有上限，超時仍往下走、靠驗證重試收尾），
# 再逐個「驗證不符才寫入」，失敗的下一輪重試。
LOCALE_CE_WAIT=0
while [ ! -d /data/system_ce/0 ] && [ "$LOCALE_CE_WAIT" -lt 30 ]; do
    sleep 2
    LOCALE_CE_WAIT=$((LOCALE_CE_WAIT + 1))
done

LOCALE_ROUND=0
while [ "$LOCALE_ROUND" -lt 3 ]; do
    LOCALE_FAILED=0
    ensure_app_locale com.miui.voiceassist zh-CN,zh-TW zh-CN
    ensure_app_locale com.miui.voicetrigger zh-CN,zh-TW zh-CN
    ensure_app_locale com.xiaomi.mibrain.speech zh-CN,zh-TW zh-CN
    ensure_app_locale com.xiaomi.aiasst.vision zh-CN,zh-TW zh-CN
    ensure_app_locale com.xiaomi.aiasst.service zh-CN,zh-TW zh-CN

    ensure_app_locale com.miui.contentextension zh-TW,zh-CN zh-TW
    ensure_app_locale com.miui.nextpay zh-TW,zh-CN zh-TW
    ensure_app_locale com.miui.tsmclient zh-TW,zh-CN zh-TW
    ensure_app_locale com.unionpay.tsmservice.mi zh-TW,zh-CN zh-TW
    ensure_app_locale com.xiaomi.payment zh-TW,zh-CN zh-TW
    ensure_app_locale com.miui.gallery zh-TW,zh-CN zh-TW
    ensure_app_locale com.miui.mediaeditor zh-TW,zh-CN zh-TW
    ensure_app_locale com.android.soundrecorder zh-TW,zh-CN zh-TW
    ensure_app_locale com.android.thememanager zh-TW,zh-CN zh-TW
    [ "$LOCALE_FAILED" = "0" ] && break
    LOCALE_ROUND=$((LOCALE_ROUND + 1))
    sleep 5
done

# --- 雙喚醒冷開機保底 -------------------------------------------------------
# 1) 冷開機記憶體高峰時，MIUI 可能在 BootupReceiver 結束後數十毫秒內回收
#    com.miui.voiceassist:voice_trigger，讓 CoreAlive 內部的 bind 來不及執行。
# 2) 2026-08-21 實測定因：開機風暴裡 GSA 的 AoHD 鏈可能卡死不載入模型
#    （實測卡 13 分鐘）；killall audioserver 只會把事件送達鏈打斷（DSP 有
#    偵測、助理不起來），絕對不可 bounce。解法是小愛武裝後盯 GSA 模型，
#    逾寬限未載入就殺 GSA 的 isolated hotword process 讓系統自動重建整條
#    鏈（細節見 dualwake_boot.sh 與 AGENTS.md「Dual-wake boot race」）。
DUALWAKE_LOG="$MODDIR/dualwake_boot.log"
if [ "$(settings get global voice_trigger_enabled)" = "1" ]; then
    case "$(settings get secure voice_interaction_service)" in
    com.miui.voiceassist/*)
        : # 小愛已是預設助理：官方 VoiceInteractionService 鏈自己會處理
        ;;
    *)
        # 獨立背景 worker，service.sh 本體立刻結束：避免開機後數分鐘
        # （支付 App 首次啟動的敏感窗口）系統裡常駐一個 cmdline 帶
        # /data/adb/modules 路徑的 root shell。worker 複製到 /data/local/tmp
        # 執行，記錄檔路徑走環境變數，不出現在 cmdline。
        WORKER_TMP=/data/local/tmp/jrc_dualwake_boot.sh
        cp "$MODDIR/dualwake_boot.sh" "$WORKER_TMP"
        DUALWAKE_LOG="$DUALWAKE_LOG" nohup sh "$WORKER_TMP" >/dev/null 2>&1 &
        ;;
    esac
fi
