#!/usr/bin/env bash
# =============================================================================
#  在 WSL 里构建 ncnn 的 onnx2ncnn 转换工具
#
#  为什么在 WSL 编：
#    onnx2ncnn 需要 protobuf。Linux 上 apt 一行就有（protobuf-compiler + libprotobuf-dev），
#    Windows 上要自己编 protobuf 或用 vcpkg，成本高得多。
#    转换属于"宿主机上的离线步骤"，Linux 二进制完全够用。
#
#  用法：
#    wsl -d Ubuntu-24.04 -u root -- bash /mnt/d/.../scripts/build-ncnn-tools-wsl.sh
#
#  产物：/opt/ncnn-tools/ncnn-build/tools/onnx/onnx2ncnn
# =============================================================================
set -euo pipefail

SRC_SRC="$(cd "$(dirname "$0")/.." && pwd)/third_party/ncnn-src"
WORK="${WORK:-/opt/ncnn-tools}"
SRC="$WORK/ncnn-src"
BUILD="$WORK/ncnn-build"

echo "==> [1/4] 安装 protobuf"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    build-essential cmake ninja-build protobuf-compiler libprotobuf-dev >/dev/null

echo "==> [2/4] 复制源码到 ext4（在 /mnt/d 上编会慢很多）"
mkdir -p "$WORK"
if [ ! -d "$SRC/CMakeLists.txt" ] && [ ! -f "$SRC/CMakeLists.txt" ]; then
    rm -rf "$SRC"
    cp -a "$SRC_SRC" "$SRC"
fi

echo "==> [3/4] 配置（只编工具，不开 Vulkan/OpenMP）"
cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DNCNN_VULKAN=OFF \
    -DNCNN_OPENMP=OFF \
    -DNCNN_BUILD_TOOLS=ON \
    -DNCNN_BUILD_EXAMPLES=OFF \
    -DNCNN_BUILD_BENCHMARK=OFF \
    -DNCNN_BUILD_TESTS=OFF \
    -DCMAKE_INSTALL_PREFIX="$WORK/install" >/dev/null

echo "==> [4/4] 编译 onnx2ncnn"
cmake --build "$BUILD" --target onnx2ncnn -j "$(nproc)"

TOOL="$BUILD/tools/onnx/onnx2ncnn"
if [ ! -x "$TOOL" ]; then
    echo "!! 没找到 onnx2ncnn"
    ls -l "$BUILD/tools" 2>/dev/null || true
    exit 2
fi

echo "==> 完成: $TOOL"
"$TOOL" 2>&1 | head -5 || true
