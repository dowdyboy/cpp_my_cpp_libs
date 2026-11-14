#!/usr/bin/env bash
# 在 WSL 里用 AddressSanitizer + UBSan 跑一遍单元测试
# 目的：在拿到板子之前，先把内存/未定义行为问题挖出来
set -euo pipefail

SRC=/mnt/d/syncworkspace/cpp_my_cpp_libs
BUILD=/tmp/mylib-asan

echo "==> 配置（Debug + ASan + UBSan）"
cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
    -DMYLIB_BUILD_EXAMPLES=OFF \
    -DMYLIB_BUILD_TOOLS=OFF \
    -DMYLIB_ENABLE_CV=OFF \
    -DMYLIB_ENABLE_NCNN=OFF >/dev/null

echo "==> 编译"
cmake --build "$BUILD" -j "$(nproc)" 2>&1 | tail -3

echo "==> 运行测试"
cd "$BUILD"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0 \
    ./bin/mylib_tests 2>&1 | tail -40
