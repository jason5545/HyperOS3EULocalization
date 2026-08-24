#!/bin/sh
# Build & run the host-side mock test for the zygisk module decision logic.
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CXX=${CXX:-c++}

# main.cpp 引用 gen/obf_strings.h（編碼字串表），測試編譯前同步產生。
python3 "$DIR/../gen_obf_strings.py" > /dev/null

# 測試需要檢查 log 輸出，故以 TAPLUS_DEBUG_LOG 編譯（release 版會在編譯期
# 移除全部 log 字串；見 main.cpp 的 TAPLUS_DEBUG_LOG 區塊）。
"$CXX" -std=c++17 -w \
    -DTAPLUS_DEBUG_LOG \
    -I"$DIR/mock" \
    "$DIR/test_main.cpp" \
    -o "$DIR/test_runner"

"$DIR/test_runner"
