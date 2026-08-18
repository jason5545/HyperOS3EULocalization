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
# 小愛使用簡中；其餘新增 App 使用繁中（台灣）。
BOOT_WAIT=0
while [ "$(getprop sys.boot_completed)" != "1" ] && [ "$BOOT_WAIT" -lt 120 ]; do
    sleep 2
    BOOT_WAIT=$((BOOT_WAIT + 1))
done

# CorePatch 存在時，由 systemless 掛載與它處理 xiaomi.eu / 國行簽章差異。
# 沒有 CorePatch 時，改用 mikal fork 的開機後 pm install 補裝流程。
force_install_cn_media() {
    INSTALL_TMP=/data/local/tmp/jrc_cn_media_install
    INSTALL_MARKER="$MODDIR/system/etc/localization/.cn_media_pm_installed_$SYSTEM_VERSION"
    INSTALL_LOG="$MODDIR/cn_media_install.log"

    if [ -f "$INSTALL_MARKER" ]; then
        return
    fi

    mkdir -p "$INSTALL_TMP"
    echo "=== CN media fallback: $(date) ===" > "$INSTALL_LOG"

    for APK_PATH in \
        "$MODDIR/system/product/priv-app/MiuiGallery/MIUIGallery.apk" \
        "$MODDIR/system/product/app/MiMediaEditor/MiMediaEditor.apk" \
        "$MODDIR/system/product/priv-app/SoundRecorder/SoundRecorder.apk" \
        "$MODDIR/system/product/app/ThemeManager/ThemeManager.apk"; do
        if [ ! -f "$APK_PATH" ]; then
            echo "MISSING: $APK_PATH" >> "$INSTALL_LOG"
            continue
        fi

        APK_NAME="$(basename "$APK_PATH")"
        TMP_APK="$INSTALL_TMP/$APK_NAME"
        cp "$APK_PATH" "$TMP_APK"

        if pm install -r -d -g "$TMP_APK" >> "$INSTALL_LOG" 2>&1; then
            echo "SUCCESS: $APK_NAME" >> "$INSTALL_LOG"
        elif pm install -d -g "$TMP_APK" >> "$INSTALL_LOG" 2>&1; then
            echo "SUCCESS (fresh): $APK_NAME" >> "$INSTALL_LOG"
        else
            echo "FAILED: $APK_NAME" >> "$INSTALL_LOG"
        fi

        rm -f "$TMP_APK"
    done

    rm -rf "$INSTALL_TMP"
    touch "$INSTALL_MARKER"
}

if ! pm path org.lsposed.corepatch >/dev/null 2>&1; then
    force_install_cn_media
fi

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
