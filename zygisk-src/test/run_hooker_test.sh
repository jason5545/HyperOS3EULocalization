#!/bin/sh
# Build & run the host-side JVM regression test for the in-app hookers
# (jrc.homefeed.*, jrc.mmedit.*). Android APIs are replaced by hand-rolled
# stubs under hooker/stub/, hook targets by hooker/fake/.
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILD_DIR="$DIR/hooker/build"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

# 單次 javac 編完：stub 取代 android.jar，順便驗證正式 hooker 只用得著這些面。
javac -d "$BUILD_DIR" \
    $(find "$DIR/hooker/stub" "$DIR/hooker/fake" -name '*.java') \
    "$DIR/../java/jrc/homefeed/HomeRsaHooker.java" \
    "$DIR/../java/jrc/homefeed/MinusScreenHooker.java" \
    "$DIR/../java/jrc/homefeed/WidgetPickerHooker.java" \
    "$DIR/../java/jrc/mmedit/RegionHooker.java" \
    "$DIR/hooker/HookerTestMain.java"

java -cp "$BUILD_DIR" HookerTestMain
