#!/bin/sh

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NDK_BUILD=${NDK_BUILD:-}

if [ -z "$NDK_BUILD" ]; then
    echo "請用 NDK_BUILD=/path/to/ndk-build 指定 Android NDK。" >&2
    exit 1
fi

"$NDK_BUILD" -B \
    NDK_PROJECT_PATH="$ROOT_DIR/zygisk-src" \
    APP_BUILD_SCRIPT="$ROOT_DIR/zygisk-src/Android.mk" \
    NDK_APPLICATION_MK="$ROOT_DIR/zygisk-src/Application.mk"
mkdir -p "$ROOT_DIR/zygisk"
cp "$ROOT_DIR/zygisk-src/libs/arm64-v8a/libtaplus_intl_fix.so" \
    "$ROOT_DIR/zygisk/arm64-v8a.so"

echo "$ROOT_DIR/zygisk/arm64-v8a.so"
