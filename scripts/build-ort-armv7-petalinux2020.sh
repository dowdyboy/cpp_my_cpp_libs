#!/usr/bin/env bash
# =============================================================================
#  用 PetaLinux 2020.2 SDK 交叉编译 ONNX Runtime 1.18.1（Zynq-7020）
#
#  【必须在 VMware 虚拟机上跑】——SDK 装在那台机器上。
#
#  为什么是 1.18.1：ORT 1.19+ 要求 GCC >= 11，而 SDK 是 GCC 9.2。
#
#  前置（都已就绪，见 docs/06）：
#    ~/Projects/cpp_my_cpp_libs/deps/onnxruntime-1.18   源码
#    ~/Projects/cpp_my_cpp_libs/deps/eigen-3.4.0        Eigen（ORT 自带的下载在虚拟机里不稳）
#    ~/Tools/{cmake,ninja,miniconda3}                   构建工具
#
#  产物：
#    ~/Projects/cpp_my_cpp_libs/dist/onnxruntime-armv7-petalinux2020/{include,lib}
#
#  用法：
#    bash scripts/build-ort-armv7-petalinux2020.sh          # 在虚拟机里
# =============================================================================
set -euo pipefail

P="${VM_PROJ:-$HOME/Projects/cpp_my_cpp_libs}"
SDK="${VM_SDK:-/home/lfbhlx/Projects/adda_fft/linux/sdk}"
CMAKE="${VM_CMAKE:-$HOME/Tools/cmake/bin/cmake}"
PYTHON="${VM_PYTHON:-$HOME/Tools/miniconda3/bin/python}"

ORT_SRC="$P/deps/onnxruntime-1.18"
EIGEN="$P/deps/eigen-3.4.0"
ORT_BUILD="$P/build/ort118"
PKG="$P/dist/onnxruntime-armv7-petalinux2020"
TOOLCHAIN="$P/src/cmake/toolchains/petalinux-2020.2-zynq.cmake"

[ -d "$ORT_SRC" ] || { echo "缺少 ORT 源码: $ORT_SRC"; exit 1; }
[ -d "$EIGEN" ]   || { echo "缺少 Eigen: $EIGEN"; exit 1; }

# SDK 的编译器在 PATH 上
source "$SDK/environment-setup-cortexa9t2hf-neon-xilinx-linux-gnueabi" >/dev/null 2>&1

echo "==> 构建（约 40 分钟）"
cd "$ORT_SRC"
"$PYTHON" tools/ci_build/build.py \
    --config Release \
    --build_shared_lib \
    --parallel 6 \
    --skip_tests \
    --compile_no_warning_as_error \
    --allow_running_as_root \
    --cmake_generator Ninja \
    --cmake_path "$CMAKE" \
    --update --build \
    --build_dir "$ORT_BUILD" \
    --cmake_extra_defines CMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    --cmake_extra_defines onnxruntime_USE_PREINSTALLED_EIGEN=ON \
    --cmake_extra_defines eigen_SOURCE_PATH="$EIGEN" \
    --cmake_extra_defines onnxruntime_BUILD_UNIT_TESTS=OFF

echo "==> 打包（931MB 里几乎全是调试信息，strip 后约 11MB）"
SO=$(ls "$ORT_BUILD"/Release/libonnxruntime.so.* | grep -v providers | head -1)
mkdir -p "$PKG/lib" "$PKG/include"
cp -a "$SO" "$PKG/lib/"
${CROSS_COMPILE}strip --strip-unneeded "$PKG/lib/$(basename "$SO")"
ln -sf "$(basename "$SO")" "$PKG/lib/libonnxruntime.so"
find "$ORT_SRC/include/onnxruntime" -name '*.h' -exec cp -a {} "$PKG/include/" \;
echo "1.18.1" > "$PKG/VERSION_NUMBER"

echo "==> ABI 校验（目标：GLIBC <= 2.30, GLIBCXX <= 3.4.27）"
${CROSS_COMPILE}readelf -h "$PKG/lib/$(basename "$SO")" | grep -E 'Class|Machine'
echo -n "最高 GLIBC:   "; ${CROSS_COMPILE}readelf -sW "$PKG/lib/$(basename "$SO")" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1
echo -n "最高 GLIBCXX: "; ${CROSS_COMPILE}readelf -sW "$PKG/lib/$(basename "$SO")" 2>/dev/null | grep -o 'GLIBCXX_[0-9.]*' | sort -uV | tail -1
du -sh "$PKG"
