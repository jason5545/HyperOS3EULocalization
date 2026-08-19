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
"$D8" --min-api 26 --lib "$ANDROID_JAR" --output "$BUILD_DIR/dex" \
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
cp "$ROOT_DIR/zygisk-src/vendor/lsplant/lib/liblsplant.so" \
    "$ROOT_DIR/zygisk/liblsplant.so"

echo "$ROOT_DIR/zygisk/arm64-v8a.so"
echo "$ROOT_DIR/zygisk/liblsplant.so"
