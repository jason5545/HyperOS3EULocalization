#!/bin/sh

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUTPUT_DIR=${1:-"$ROOT_DIR/dist"}
case "$OUTPUT_DIR" in
    /*) ;;
    *) OUTPUT_DIR="$(pwd)/$OUTPUT_DIR" ;;
esac

VERSION=$(sed -n 's/^version=//p' "$ROOT_DIR/module.prop")
OUTPUT_NAME="HyperOS3_EU_XiaoAI_Portal_MiPay_${VERSION}.zip"
OUTPUT_PATH="$OUTPUT_DIR/$OUTPUT_NAME"

REQUIRED_PAYLOADS="
system/product/app/VoiceAssistAndroidT
system/product/app/AiAsstVision
system/product/app/MIUIAiasstService
system/product/priv-app/MIUIContentExtension
system/product/app/MINextpay
system/product/app/MITSMClient
system/product/app/UPTsmService
system/product/app/PaymentService
system/product/priv-app/MiuiGallery
system/product/app/MiMediaEditor
system/product/priv-app/SoundRecorder
system/product/app/ThemeManager
"

EXCLUDED_PATHS="
system/product/app/HybridPlatform
system/product/app/MIUISuperMarket
system/product/app/MipayWallet
system/product/priv-app/PersonalAssistant
system/product/priv-app/Mms
system/product/priv-app/MIUIYellowPage
"

for payload in $REQUIRED_PAYLOADS; do
    if [ ! -d "$ROOT_DIR/$payload" ]; then
        echo "缺少必要 payload: $payload" >&2
        exit 1
    fi
    PAYLOAD_APK_COUNT=$(find "$ROOT_DIR/$payload" -type f -name '*.apk' | wc -l | tr -d ' ')
    if [ "$PAYLOAD_APK_COUNT" != "1" ]; then
        echo "$payload 應該正好包含 1 個 APK，目前找到 $PAYLOAD_APK_COUNT 個" >&2
        exit 1
    fi
done

for path in $EXCLUDED_PATHS; do
    if [ -e "$ROOT_DIR/$path" ]; then
        echo "發現不應打包的路徑: $path" >&2
        exit 1
    fi
done

if [ ! -f "$ROOT_DIR/zygisk/arm64-v8a.so" ]; then
    echo "缺少 Taplus Zygisk arm64 binary: zygisk/arm64-v8a.so" >&2
    exit 1
fi

if [ ! -f "$ROOT_DIR/excluded_packages.txt" ]; then
    echo "缺少 Taplus 排除清單: excluded_packages.txt" >&2
    exit 1
fi

mkdir -p "$OUTPUT_DIR"
rm -f "$OUTPUT_PATH"

cd "$ROOT_DIR"
zip -qr "$OUTPUT_PATH" \
    META-INF \
    module.prop \
    customize.sh \
    service.sh \
    uninstall.sh \
    tools/unity_install.sh \
    excluded_packages.txt \
    zygisk/arm64-v8a.so \
    $REQUIRED_PAYLOADS \
    LICENSE

echo "$OUTPUT_PATH"
