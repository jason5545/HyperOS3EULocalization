fail_install() {
    ui_print "! 安裝中止"
    rm -rf "$MODPATH"
    rm -f "$MODDIR/update"
    exit 1
}

log_item() {
    ui_print "- $1"
}

log_warn() {
    ui_print "! $1"
}

MODDIR="$NVBASE/modules/$MODID"
MODVERSION="$(grep_prop version "$MODPATH/module.prop")"
BUILDHOST="$(getprop ro.build.host)"
MIUIVERSION="$(getprop ro.system.build.version.incremental)"
HYPEROSVERSION="$(getprop ro.mi.os.version.name)"
BUILDDISPLAY="$(getprop ro.build.display.id)"

SYSTEM_VERSION=${MIUIVERSION:-unknown}

log_item "檢查安裝環境"

if [ "${BOOTMODE:-false}" != "true" ]; then
    log_warn "請從 Magisk、KernelSU、SukiSU 或 APatch 的模組管理器安裝。"
    fail_install
fi

UPSTREAM_MODULE="$NVBASE/modules/HyperOS3EULocalization"
if [ -d "$UPSTREAM_MODULE" ] && [ ! -e "$UPSTREAM_MODULE/disable" ]; then
    log_warn "偵測到原版 HyperOS3EULocalization，請先停用或移除，避免兩個模組掛載同一路徑。"
    fail_install
fi

STANDALONE_TAPLUS="$NVBASE/modules/taplus_intl_fix"
if [ -d "$STANDALONE_TAPLUS" ] && [ ! -e "$STANDALONE_TAPLUS/disable" ] && [ ! -e "$STANDALONE_TAPLUS/remove" ]; then
    log_warn "偵測到獨立 taplus_intl_fix；本模組已內建相同 hook，安裝後請移除舊模組。"
fi

if [ "$BUILDHOST" != "xiaomi.eu" ]; then
    log_warn "未偵測到 xiaomi.eu build host，仍會繼續安裝。"
fi

case "$MIUIVERSION $HYPEROSVERSION $BUILDDISPLAY" in
    *OS3*|*os3*) ;;
    *)
        case "$HYPEROSVERSION" in
            3*) ;;
            *) log_warn "未偵測到 HyperOS 3，仍會繼續安裝。" ;;
        esac
        ;;
esac

REQUIRED_PAYLOADS="
system/product/app/VoiceAssistAndroidT
system/product/app/AiAsstVision
system/product/app/MIUIAiasstService
system/product/priv-app/MIUIContentExtension
system/product/app/MINextpay
system/product/app/MITSMClient
system/product/app/UPTsmService
system/product/app/PaymentService
system/product/app/ThemeManager
"

DATA_APP_PAYLOADS="
payload/cn-media/MiuiGallery.apk
payload/cn-media/MiMediaEditor.apk
payload/cn-media/SoundRecorder.apk
payload/cn-media/MiuiThemeManagerCnOverlay.apk
"

log_item "檢查固定 payload"
for payload in $REQUIRED_PAYLOADS; do
    if [ ! -d "$MODPATH/$payload" ]; then
        log_warn "缺少 $payload"
        fail_install
    fi
done

for payload in $DATA_APP_PAYLOADS; do
    if [ ! -f "$MODPATH/$payload" ]; then
        log_warn "缺少 $payload"
        fail_install
    fi
done

# v1.0.2 早期版本曾包含小米錢包；更新時明確清掉舊 payload。
rm -rf "$MODPATH/system/product/app/MipayWallet"

# ThemeManager 在 MYRON xiaomi.eu 的原始路徑是 /product/app；清掉其他 fork
# 使用的 /system/app 位置，避免同包名兩份 APK 並存。
rm -rf "$MODPATH/system/app/ThemeManager"

# 舊測試版曾把三個 App 直接放進 systemless tree 並以 post-fs-data bind。
# 更新時必須移除，避免支付 App 看見額外 mount，也避免 hybrid_mount 空 root 報錯。
rm -rf \
    "$MODPATH/system/product/priv-app/MiuiGallery" \
    "$MODPATH/system/product/app/MiMediaEditor" \
    "$MODPATH/system/product/priv-app/SoundRecorder" \
    "$MODPATH/system/product/data-app"
rm -f \
    "$MODPATH/system/product/overlay/MiuiThemeManagerCnOverlay.apk" \
    "$MODPATH/post-fs-data.sh" \
    "$MODPATH/mount_error"
rmdir "$MODPATH/system/product/overlay" 2>/dev/null || true

if [ ! -f "$MODPATH/zygisk/arm64-v8a.so" ]; then
    log_warn "缺少 Taplus Zygisk arm64 binary"
    fail_install
fi

if [ ! -f "$MODPATH/excluded_packages.txt" ]; then
    log_warn "缺少 Taplus Zygisk 排除清單"
    fail_install
fi

mkdir -p "$MODPATH/system/etc/localization/SystemVersion"
touch "$MODPATH/system/etc/localization/XiaoAI"
touch "$MODPATH/system/etc/localization/AICall"
touch "$MODPATH/system/etc/localization/ContentExtension"
touch "$MODPATH/system/etc/localization/Mipay"
touch "$MODPATH/system/etc/localization/Gallery"
touch "$MODPATH/system/etc/localization/MediaEditor"
touch "$MODPATH/system/etc/localization/SoundRecorder"
touch "$MODPATH/system/etc/localization/ThemeManager"
touch "$MODPATH/system/etc/localization/SystemVersion/$SYSTEM_VERSION"

# Mi Pay 的安全元件類型與小愛服務開關。
# 地區與 mod_device 完全沿用 ROM，不強迫切到 CN。
cat > "$MODPATH/system.prop" <<EOF
ro.se.type=eSE,HCE,UICC
ro.vendor.audio.aiasst.support=true
jason.hyperos3.eu.core=$MODVERSION
EOF

rm -rf /data/system/package_cache/*

if ! pm path org.lsposed.corepatch >/dev/null 2>&1; then
    log_warn "未偵測到 CorePatch；ThemeManager 的 shared UID／簽章相容可能失敗。"
fi

log_item "已安裝：小愛、AI 通話、傳送門、智慧卡支付鏈路"
log_item "已準備：國行相簿、編輯器、錄音機正常安裝 payload 與 ThemeManager systemless payload"
log_item "已加入：AI 通話官方入口與 cloud-control、Taplus、Theme API region、Wallet/GMS 安全排除與 App 語系設定"
