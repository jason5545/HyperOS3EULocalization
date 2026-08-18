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
# 小愛使用簡中；傳送門與智慧卡支付鏈路使用繁中（台灣）。
BOOT_WAIT=0
while [ "$(getprop sys.boot_completed)" != "1" ] && [ "$BOOT_WAIT" -lt 120 ]; do
    sleep 2
    BOOT_WAIT=$((BOOT_WAIT + 1))
done

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
