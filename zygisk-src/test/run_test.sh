#!/bin/sh
# Build & run the host-side mock test for the zygisk module decision logic.
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CXX=${CXX:-c++}

"$CXX" -std=c++17 -w \
    -I"$DIR/mock" \
    "$DIR/test_main.cpp" \
    -o "$DIR/test_runner"

"$DIR/test_runner"
