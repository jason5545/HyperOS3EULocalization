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
# 小愛與 AI 通話使用簡中；其餘新增 App 使用繁中（台灣）。
BOOT_WAIT=0
while [ "$(getprop sys.boot_completed)" != "1" ] && [ "$BOOT_WAIT" -lt 120 ]; do
    sleep 2
    BOOT_WAIT=$((BOOT_WAIT + 1))
done

# Gallery / MediaEditor / SoundRecorder 走正常 /data/app 安裝，讓 Android
# 自己解出 native libraries；不要建立額外 bind mount，避免支付 App 看見
# KernelSU 模組掛載。ThemeManager 因 shared UID/重複 permission 保留 systemless。
MEDIA_PAYLOAD_DIR="$MODDIR/payload/cn-media"
MEDIA_INSTALL_TMP=/data/local/tmp/jrc_cn_media_install
MEDIA_INSTALL_LOG="$MODDIR/cn_media_install.log"

installed_version_code() {
    dumpsys package "$1" 2>/dev/null \
        | sed -n 's/.*versionCode=\([0-9][0-9]*\).*/\1/p' \
        | head -n 1
}

ensure_data_app() {
    PACKAGE_NAME="$1"
    APK_NAME="$2"
    EXPECTED_VERSION="$3"
    APK_PATH="$MEDIA_PAYLOAD_DIR/$APK_NAME"

    if [ "$(installed_version_code "$PACKAGE_NAME")" = "$EXPECTED_VERSION" ]; then
        return
    fi
    if [ ! -f "$APK_PATH" ]; then
        echo "MISSING: $APK_PATH" >> "$MEDIA_INSTALL_LOG"
        return
    fi

    mkdir -p "$MEDIA_INSTALL_TMP"
    TMP_APK="$MEDIA_INSTALL_TMP/$APK_NAME"
    cp "$APK_PATH" "$TMP_APK"
    if pm install -r -d -g "$TMP_APK" >> "$MEDIA_INSTALL_LOG" 2>&1; then
        echo "SUCCESS: $PACKAGE_NAME -> $EXPECTED_VERSION" >> "$MEDIA_INSTALL_LOG"
    else
        echo "FAILED: $PACKAGE_NAME -> $EXPECTED_VERSION" >> "$MEDIA_INSTALL_LOG"
    fi
    rm -f "$TMP_APK"
}

echo "=== CN media ensure: $(date) ===" > "$MEDIA_INSTALL_LOG"
ensure_data_app com.miui.gallery MiuiGallery.apk 5000507
ensure_data_app com.miui.mediaeditor MiMediaEditor.apk 203990083
ensure_data_app com.android.soundrecorder SoundRecorder.apk 708093

# 官方 308 static overlay 在 hybrid_mount 環境可能無法新增到 /product/overlay；
# 改由 Package Manager 安裝並讓 OverlayManager 啟用。
THEME_OVERLAY_PACKAGE=com.android.thememanager.customthemeconfig.config.overlay
THEME_OVERLAY_APK="$MEDIA_PAYLOAD_DIR/MiuiThemeManagerCnOverlay.apk"
if ! pm path "$THEME_OVERLAY_PACKAGE" >/dev/null 2>&1 && [ -f "$THEME_OVERLAY_APK" ]; then
    mkdir -p "$MEDIA_INSTALL_TMP"
    cp "$THEME_OVERLAY_APK" "$MEDIA_INSTALL_TMP/MiuiThemeManagerCnOverlay.apk"
    pm install -r -d -g "$MEDIA_INSTALL_TMP/MiuiThemeManagerCnOverlay.apk" \
        >> "$MEDIA_INSTALL_LOG" 2>&1
    rm -f "$MEDIA_INSTALL_TMP/MiuiThemeManagerCnOverlay.apk"
fi
cmd overlay enable --user 0 "$THEME_OVERLAY_PACKAGE" >/dev/null 2>&1
rm -rf "$MEDIA_INSTALL_TMP"

set_app_locale() {
    PACKAGE_NAME="$1"
    LOCALE_CONFIG="$2"
    APP_LOCALE="$3"
    cmd locale set-app-localeconfig "$PACKAGE_NAME" --user 0 --locales "$LOCALE_CONFIG" >/dev/null 2>&1
    cmd locale set-app-locales "$PACKAGE_NAME" --user 0 --locales "$APP_LOCALE" >/dev/null 2>&1
}

set_app_locale com.miui.voiceassist zh-CN,zh-TW zh-CN
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
