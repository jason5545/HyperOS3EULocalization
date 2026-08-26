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
system/product/app/VoiceTrigger
system/product/app/AiAsstVision
system/product/app/MIUIAiasstService
system/product/priv-app/MIUIContentExtension
system/product/priv-app/SoundRecorder
system/product/app/MINextpay
system/product/app/MITSMClient
system/product/app/UPTsmService
system/product/app/PaymentService
system/product/app/ThemeManager
system/product/app/MiMediaEditor
system/product/priv-app/MiuiHome
system/product/overlay/VoiceAssistAndroidOverlay
"

DATA_APP_PAYLOADS="
payload/cn-media/MiuiGallery.apk
payload/cn-media/MiuiThemeManagerCnOverlay.apk
payload/xiaoai/MIUIXiaoAiSpeechEngine.apk
"

# 單檔 systemless payload：priv-app 授權 XML（補 CN 有、EU 缺的 grants）、
# 雙喚醒冷開機保底 worker、敏感進程 mount 清道夫與 AI 通話入口預設開啟
# （worker 皆由 service.sh 複製到 /data/local/tmp 執行）
FILE_PAYLOADS="
system/product/etc/permissions/privapp-permissions-hyperos3eu.xml
dualwake_boot.sh
mount_scrub.sh
aicall_defaulton.sh
"

EXCLUDED_PATHS="
system/product/app/HybridPlatform
system/product/app/MIUISuperMarket
system/product/app/MipayWallet
system/product/priv-app/PersonalAssistant
system/product/priv-app/Mms
system/product/priv-app/MIUIYellowPage
system/product/priv-app/MiuiGallery
system/product/data-app
post-fs-data.sh
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

for payload in $DATA_APP_PAYLOADS $FILE_PAYLOADS; do
    if [ ! -f "$ROOT_DIR/$payload" ]; then
        echo "缺少必要單檔 payload: $payload" >&2
        exit 1
    fi
done

# KSU Next hybrid mount 是唯讀 overlay，PackageManager 無法在掃描時現場解出
# APK 內的 native lib（extractNativeLibs=true 的 App 會直接 UnsatisfiedLinkError）。
# 凡 APK 內含 lib/arm64-v8a/*.so 的 payload，必須附上預解壓的 lib/arm64/。
for payload in $REQUIRED_PAYLOADS; do
    PAYLOAD_APK=$(find "$ROOT_DIR/$payload" -maxdepth 1 -type f -name '*.apk')
    MISSING_LIBS=""
    for so in $(unzip -Z1 "$PAYLOAD_APK" 'lib/arm64-v8a/*.so' 2>/dev/null | sed 's|.*/||'); do
        if [ ! -f "$ROOT_DIR/$payload/lib/arm64/$so" ]; then
            MISSING_LIBS="$MISSING_LIBS $so"
        fi
    done
    if [ -n "$MISSING_LIBS" ]; then
        echo "$payload 的 APK 內含 native lib 但缺少預解壓檔:$MISSING_LIBS" >&2
        echo "請以 unzip -j 解出 lib/arm64-v8a/* 到 $payload/lib/arm64/" >&2
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

if [ ! -f "$ROOT_DIR/zygisk/liblsplant.so" ]; then
    echo "缺少 VoiceTrigger hook 用的 LSPlant runtime: zygisk/liblsplant.so" >&2
    exit 1
fi

if [ ! -f "$ROOT_DIR/excluded_packages.txt" ]; then
    echo "缺少 Taplus 排除清單: excluded_packages.txt" >&2
    exit 1
fi

# payload_versions.txt：所有 payload APK 的「package versionCode 模組相對路徑」
# 清單，供 service.sh 做 data-app 期望版本查詢與系統 App 登錄稽核（PM 嚴格
# 升級保留的自愈，見 AGENTS.md「PM 嚴格升級保留」）。建置產物，不入庫。
AAPT2=$(ls -d "$HOME"/Library/Android/sdk/build-tools/*/aapt2 2>/dev/null | sort -V | tail -1)
if [ -z "$AAPT2" ]; then
    echo "找不到 aapt2（需要 Android SDK build-tools）" >&2
    exit 1
fi
VERSIONS_FILE="$ROOT_DIR/payload_versions.txt"
: > "$VERSIONS_FILE"
for payload in $REQUIRED_PAYLOADS; do
    PAYLOAD_APK=$(find "$ROOT_DIR/$payload" -maxdepth 1 -type f -name '*.apk')
    BADGING=$("$AAPT2" dump badging "$PAYLOAD_APK" | head -1)
    PNAME=$(printf '%s' "$BADGING" | sed -n "s/^package: name='\([^']*\)'.*/\1/p")
    PVC=$(printf '%s' "$BADGING" | sed -n "s/^package: name='[^']*' versionCode='\([0-9]*\)'.*/\1/p")
    if [ -z "$PNAME" ] || [ -z "$PVC" ]; then
        echo "aapt2 解析失敗: $PAYLOAD_APK" >&2
        exit 1
    fi
    echo "$PNAME $PVC $payload/${PAYLOAD_APK##*/}" >> "$VERSIONS_FILE"
done
for payload in $DATA_APP_PAYLOADS; do
    BADGING=$("$AAPT2" dump badging "$ROOT_DIR/$payload" | head -1)
    PNAME=$(printf '%s' "$BADGING" | sed -n "s/^package: name='\([^']*\)'.*/\1/p")
    PVC=$(printf '%s' "$BADGING" | sed -n "s/^package: name='[^']*' versionCode='\([0-9]*\)'.*/\1/p")
    if [ -z "$PNAME" ] || [ -z "$PVC" ]; then
        echo "aapt2 解析失敗: $payload" >&2
        exit 1
    fi
    echo "$PNAME $PVC $payload" >> "$VERSIONS_FILE"
done

mkdir -p "$OUTPUT_DIR"
rm -f "$OUTPUT_PATH"

cd "$ROOT_DIR"
zip -qr "$OUTPUT_PATH" \
    META-INF/com/google/android/update-binary \
    META-INF/com/google/android/updater-script \
    module.prop \
    customize.sh \
    service.sh \
    uninstall.sh \
    tools/unity_install.sh \
    excluded_packages.txt \
    payload_versions.txt \
    zygisk/arm64-v8a.so \
    zygisk/liblsplant.so \
    $REQUIRED_PAYLOADS \
    $DATA_APP_PAYLOADS \
    $FILE_PAYLOADS \
    LICENSE

echo "$OUTPUT_PATH"
