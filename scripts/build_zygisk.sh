#!/bin/sh

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NDK_BUILD=${NDK_BUILD:-}
ANDROID_HOME=${ANDROID_HOME:-"$HOME/Library/Android/sdk"}

if [ -z "$NDK_BUILD" ]; then
    echo "請用 NDK_BUILD=/path/to/ndk-build 指定 Android NDK。" >&2
    exit 1
fi

# --- 1. 編譯內嵌 dex（CoreAlive bridge + VoiceTrigger restart hooker） ---
ANDROID_JAR=$(ls -d "$ANDROID_HOME"/platforms/android-*/android.jar 2>/dev/null | sort -V | tail -1)
D8=$(ls -d "$ANDROID_HOME"/build-tools/*/d8 2>/dev/null | sort -V | tail -1)
if [ ! -f "$ANDROID_JAR" ] || [ ! -x "$D8" ]; then
    echo "缺少 Android SDK platform 或 d8（ANDROID_HOME=$ANDROID_HOME）" >&2
    exit 1
fi

JAVA_SRC_DIR="$ROOT_DIR/zygisk-src/java"
BUILD_DIR="$ROOT_DIR/zygisk-src/build"
GEN_DIR="$ROOT_DIR/zygisk-src/gen"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR/classes" "$BUILD_DIR/dex" "$GEN_DIR"

# shellcheck disable=SC2086
javac -source 8 -target 8 -bootclasspath "$ANDROID_JAR" \
    -d "$BUILD_DIR/classes" \
    $(find "$JAVA_SRC_DIR" -name '*.java')

# shellcheck disable=SC2086
"$D8" --release --min-api 26 --lib "$ANDROID_JAR" --output "$BUILD_DIR/dex" \
    $(find "$BUILD_DIR/classes" -name '*.class')

xxd -i -n hooker_dex "$BUILD_DIR/dex/classes.dex" > "$GEN_DIR/hooker_dex.h"

# --- 2. 編譯 Zygisk native library ---
"$NDK_BUILD" -B \
    NDK_PROJECT_PATH="$ROOT_DIR/zygisk-src" \
    APP_BUILD_SCRIPT="$ROOT_DIR/zygisk-src/Android.mk" \
    NDK_APPLICATION_MK="$ROOT_DIR/zygisk-src/Application.mk"
mkdir -p "$ROOT_DIR/zygisk"
cp "$ROOT_DIR/zygisk-src/libs/arm64-v8a/libtaplus_intl_fix.so" \
    "$ROOT_DIR/zygisk/arm64-v8a.so"

# 降痕：strip 符號表與 .gnu_debugdata（mini debug info）。模組 .so 會在
# 未排除 app 的 /proc/self/maps 裡出現，內容字串（含編譯主機路徑）可被
# 該 app 直接讀取；dlclose 只對敏感／金融／排除進程生效，其餘進程仍常駐。
# strip 不影響 .dynsym 中的 zygisk_module_entry，dlopen 不受影響。
NDK_DIR=$(CDPATH= cd -- "$(dirname -- "$NDK_BUILD")" && pwd)
LLVM_STRIP=$(ls "$NDK_DIR"/toolchains/llvm/prebuilt/*/bin/llvm-strip 2>/dev/null | head -1)
if [ -n "$LLVM_STRIP" ]; then
    "$LLVM_STRIP" --strip-all --remove-section=.gnu_debugdata \
        "$ROOT_DIR/zygisk/arm64-v8a.so"
else
    echo "警告：找不到 llvm-strip，未 strip $ROOT_DIR/zygisk/arm64-v8a.so" >&2
fi

cp "$ROOT_DIR/zygisk-src/vendor/lsplant/lib/liblsplant.so" \
    "$ROOT_DIR/zygisk/liblsplant.so"

echo "$ROOT_DIR/zygisk/arm64-v8a.so"
echo "$ROOT_DIR/zygisk/liblsplant.so"
