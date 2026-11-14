#!/usr/bin/env bash
# =============================================================================
#  在 WSL(Ubuntu 24.04) 中交叉编译 ONNX Runtime for armv7-A (Zynq-7000 / Cortex-A9)
#
#  为什么必须在 Linux 里编：
#    ONNX Runtime 的构建系统假定"Linux 宿主 -> Linux ARM 目标"，
#    官方不支持 mingw 宿主交叉编译。WSL 是最省事的宿主。
#
#  为什么用 Ubuntu 的 armhf 工具链而不是 PetaLinux SDK：
#    PetaLinux 2024.2 的 sysroot glibc = 2.39，Ubuntu 24.04 armhf 也是 2.39；
#    两者 libstdc++ 都是 GCC 13.x（6.0.32）。用宿主发行版自带的交叉工具链
#    可以避免在 WSL 里挂 Windows 路径的 SDK。
#    若目标板 glibc 更老，请把 SYSROOT 指向板子自己的 sysroot。
#
#  用法（Windows 侧）：
#    wsl -d Ubuntu-24.04 -u root -- bash /mnt/d/.../scripts/build-ort-armv7-wsl.sh
#
#  产物：
#    /opt/ort-build/install/lib/libonnxruntime.so
#    /opt/ort-build/install/include/*.h
# =============================================================================
set -euo pipefail

ORT_VERSION="${ORT_VERSION:-v1.23.2}"
WORK="${WORK:-/opt/ort-build}"
SRC="$WORK/onnxruntime"
BUILD="$WORK/build"
INSTALL="$WORK/install"
LOG="$WORK/build.log"

echo "==> [0/5] 网络代理"
# WSL2 的 NAT 模式下 127.0.0.1 指向 WSL 自己，Windows 上的代理只能用默认网关 IP 访问。
# 这里自动探测一次，能通就设置环境变量，后续 git clone / CMake FetchContent 都会走代理。
GW=$(ip route show default | awk '{print $3}')
PROXY_PORT="${PROXY_PORT:-7897}"
if [ -n "$GW" ] && curl -s -o /dev/null --max-time 5 -x "http://${GW}:${PROXY_PORT}" https://github.com; then
    export http_proxy="http://${GW}:${PROXY_PORT}"
    export https_proxy="$http_proxy"
    export HTTP_PROXY="$http_proxy"
    export HTTPS_PROXY="$http_proxy"
    echo "    使用代理 $http_proxy"
else
    echo "    未探测到可用代理，直连"
fi

echo "==> [1/5] 安装依赖"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    build-essential cmake ninja-build pkg-config git curl ca-certificates \
    python3 python3-pip python3-venv \
    gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf \
    file binutils-arm-linux-gnueabihf >/dev/null

# ORT 的 build.py 需要这些 python 包
pip3 install --quiet --break-system-packages \
    numpy packaging flatbuffers protobuf sympy onnx 2>/dev/null || \
  pip3 install --quiet numpy packaging flatbuffers protobuf sympy onnx

echo "==> [2/5] 获取源码 $ORT_VERSION"
mkdir -p "$WORK"
if [ ! -d "$SRC/.git" ]; then
    git clone --depth 1 --branch "$ORT_VERSION" \
        --recurse-submodules --shallow-submodules \
        https://github.com/microsoft/onnxruntime.git "$SRC"
else
    echo "    已存在，跳过 clone"
fi

echo "==> [3/5] 生成 armv7 工具链文件"
cat > "$WORK/armv7-toolchain.cmake" <<'EOF'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER   arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

# Cortex-A9: armv7-A + NEON + 硬浮点
set(CMAKE_C_FLAGS_INIT   "-march=armv7-a -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fPIC")
set(CMAKE_CXX_FLAGS_INIT "-march=armv7-a -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fPIC")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF

echo "==> [4/5] 开始构建（预计 20~45 分钟，日志 $LOG）"
cd "$SRC"
set +e
# 注意：ORT 1.23 的 build.py 已经删掉了 --arm/--arm64 参数，
# 交叉编译完全由 CMAKE_TOOLCHAIN_FILE 驱动（build.py 检测到交叉编译后
# 默认只做 update+build，不跑测试）。
./build.sh \
    --config Release \
    --build_shared_lib \
    --parallel \
    --skip_tests \
    --compile_no_warning_as_error \
    --allow_running_as_root \
    --update \
    --build \
    --build_dir "$BUILD" \
    --cmake_extra_defines CMAKE_TOOLCHAIN_FILE="$WORK/armv7-toolchain.cmake" \
    --cmake_extra_defines CMAKE_BUILD_TYPE=Release \
    --cmake_extra_defines onnxruntime_BUILD_UNIT_TESTS=OFF \
    > "$LOG" 2>&1
RC=$?
set -e

if [ $RC -ne 0 ]; then
    echo "!! 构建失败 (rc=$RC)，日志尾部："
    tail -60 "$LOG"
    exit $RC
fi

echo "==> [5/5] 收集产物"
mkdir -p "$INSTALL/lib" "$INSTALL/include"
SO=$(find "$BUILD" -name 'libonnxruntime.so.*.*' -type f | head -1)
if [ -z "$SO" ]; then
    SO=$(find "$BUILD" -name 'libonnxruntime.so*' -type f | head -1)
fi
if [ -z "$SO" ]; then
    echo "!! 没找到 libonnxruntime.so"
    exit 2
fi
cp -a "$SO" "$INSTALL/lib/"
ln -sf "$(basename "$SO")" "$INSTALL/lib/libonnxruntime.so"

# ORT 的头文件之间用裸文件名互相 include，必须摊平到同一层
find "$SRC/include/onnxruntime" -name '*.h' -exec cp -a {} "$INSTALL/include/" \;
cp -a "$SRC/VERSION_NUMBER" "$INSTALL/" 2>/dev/null || true

echo "==> 完成"
file "$INSTALL/lib/$(basename "$SO")"
ls -lh "$INSTALL/lib/"
echo "SONAME:"
arm-linux-gnueabihf-readelf -d "$INSTALL/lib/$(basename "$SO")" | grep SONAME
echo "最大 GLIBC 符号版本（必须 <= 目标板 glibc）:"
arm-linux-gnueabihf-readelf -sW "$INSTALL/lib/$(basename "$SO")" 2>/dev/null \
    | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1
