#!/usr/bin/env bash
# =============================================================================
#  用 PetaLinux 2020.2 SDK 交叉编译 OpenCV（Zynq-7020 / Cortex-A9）
#
#  【必须在 VMware 虚拟机上跑】——SDK 装在那台机器上。
#
#  只编三块：core / imgproc / imgcodecs。板端图像链路（读图、resize、
#  letterbox）就要这些；DNN/features2d/videoio 一概不编，体积能小一个量级。
#
#  前置：
#    ~/Projects/cpp_my_cpp_libs/deps/opencv-4.12.0   源码（或任意 4.x）
#    ~/Tools/{cmake,ninja}                           构建工具
#
#  产物：
#    ~/Projects/cpp_my_cpp_libs/dist/opencv-4.12.0-armv7/{include,lib}
#  拷回 Windows 后放到 third_party/opencv-4.12.0-armv7/ 即可被
#  cmake/Deps.cmake 自动发现（会做 ELF 架构校验）。
#
#  ---- 实测记录（2026-11-15，VM 12 核，GCC 9.2.0）----
#    * 三个 .so 合计 5.9 MB（dist 含头文件共 21 MB），构建约 3 分钟；
#    * 最高 GLIBC_2.29 / GLIBCXX_3.4.26 —— 板子 glibc 2.30，够；
#    * 整个工程（core+log+json+math+infer+image+dsp+cv，ORT/ncnn 全开）
#      在 armv7 上编译、链接通过，产物的 NEEDED 里有
#      libonnxruntime.so.1.18.1 / libopencv_core.so.412 等。
#
#  用法：
#    bash scripts/build-opencv-armv7-petalinux2020.sh
#    OPENCV_SRC=... PKG=... bash scripts/build-opencv-armv7-petalinux2020.sh
# =============================================================================
set -euo pipefail

P="${VM_PROJ:-$HOME/Projects/cpp_my_cpp_libs}"
SDK="${VM_SDK:-$HOME/Projects/adda_fft/linux/sdk}"
CMAKE="${VM_CMAKE:-$HOME/Tools/cmake/bin/cmake}"
NINJA="${VM_NINJA:-$HOME/Tools/ninja}"

OPENCV_SRC="${OPENCV_SRC:-$P/deps/opencv-4.12.0}"
OPENCV_VER="${OPENCV_VER:-4.12.0}"
BUILD="$P/build/opencv-armv7"
PKG="${PKG:-$P/dist/opencv-${OPENCV_VER}-armv7}"
TOOLCHAIN="$P/src/cmake/toolchains/petalinux-2020.2-zynq.cmake"

[ -d "$OPENCV_SRC" ] || { echo "缺少 OpenCV 源码: $OPENCV_SRC"; exit 1; }
[ -f "$TOOLCHAIN" ]  || { echo "缺少工具链文件: $TOOLCHAIN"; exit 1; }

# 工具兜底：Tools 下的路径可能被整理过，找不到就退回 PATH
[ -x "$CMAKE" ] || CMAKE="$(command -v cmake)"
[ -x "$NINJA" ] || NINJA="$(command -v ninja)"
[ -n "$CMAKE" ] || { echo "找不到 cmake"; exit 1; }
[ -n "$NINJA" ] || { echo "找不到 ninja"; exit 1; }

# SDK 只用来把交叉编译器放进 PATH（工具链文件里还有 HINTS 兜底）。
# 这个 environment-setup 脚本在 set -u 下会报未定义变量，所以临时关掉 nounset，
# 而且它失败也不致命 —— 否则脚本会在这里静默退出，日志里一个字都没有。
if [ -f "$SDK/environment-setup-cortexa9t2hf-neon-xilinx-linux-gnueabi" ]; then
    set +u
    # shellcheck disable=SC1090
    source "$SDK/environment-setup-cortexa9t2hf-neon-xilinx-linux-gnueabi" >/dev/null 2>&1 || \
        echo "（警告）SDK 环境脚本 source 失败，继续用工具链文件里的绝对路径"
    set -u
fi

echo "==> 配置（约 20~40 分钟，看虚拟机核数）"
# 说明：
#   * BUILD_LIST 只留三块，其余模块连配置都不做；
#   * 图片解码依赖 zlib/libjpeg/libpng，用 OpenCV 自带的 3rdparty 编
#     （BUILD_ZLIB/JPEG/PNG=ON），避免依赖板子 sysroot 里有没有这些库；
#   * CPU_BASELINE=NEON：Cortex-A9 有 NEON，OpenCV 的 resize/cvtColor 会用上；
#   * 关掉所有硬件/UI/多媒体后端：板子上不接摄像头、不用 GTK。
"$CMAKE" -S "$OPENCV_SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PKG" \
    -DBUILD_LIST=core,imgproc,imgcodecs \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_opencv_world=OFF \
    -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF \
    -DBUILD_opencv_apps=OFF -DBUILD_JAVA=OFF -DBUILD_PYTHON=OFF -DBUILD_opencv_python3=OFF \
    -DBUILD_ZLIB=ON -DBUILD_JPEG=ON -DBUILD_PNG=ON \
    -DWITH_TIFF=OFF -DWITH_WEBP=OFF -DWITH_OPENEXR=OFF -DWITH_JASPER=OFF \
    -DWITH_OPENCL=OFF -DWITH_IPP=OFF -DWITH_ITT=OFF -DWITH_TBB=OFF -DWITH_OPENMP=OFF \
    -DWITH_FFMPEG=OFF -DWITH_GSTREAMER=OFF -DWITH_V4L=OFF -DWITH_LIBV4L=OFF \
    -DWITH_GTK=OFF -DWITH_QT=OFF -DWITH_EIGEN=OFF -DWITH_PROTOBUF=OFF \
    -DWITH_LAPACK=OFF -DWITH_OPENBLAS=OFF -DWITH_CLP=OFF \
    -DOPENCV_GENERATE_PKGCONFIG=OFF \
    -DENABLE_NEON=ON -DCPU_BASELINE=NEON -DCPU_DISPATCH=

echo "==> 构建 + 安装"
"$CMAKE" --build "$BUILD" --parallel "$(nproc)"
"$CMAKE" --install "$BUILD"

echo "==> strip + 校验"
for so in "$PKG"/lib/libopencv_*.so.*; do
    [ -f "$so" ] || continue
    ${CROSS_COMPILE}strip --strip-unneeded "$so"
    # 补齐不带版本号的 soname 软链（CMake install 已经建了，这里兜底）
done
for m in core imgproc imgcodecs; do
    so=$(ls "$PKG"/lib/libopencv_${m}.so.* 2>/dev/null | head -1 || true)
    [ -n "$so" ] || { echo "缺少 libopencv_${m}"; exit 1; }
    echo -n "libopencv_${m}: "
    ${CROSS_COMPILE}readelf -h "$so" | grep -E 'Machine' | tr -s ' '
done

echo -n "最高 GLIBC:   "
${CROSS_COMPILE}readelf -sW "$PKG"/lib/libopencv_core.so.* 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1
echo -n "最高 GLIBCXX: "
${CROSS_COMPILE}readelf -sW "$PKG"/lib/libopencv_core.so.* 2>/dev/null | grep -o 'GLIBCXX_[0-9.]*' | sort -uV | tail -1

echo "==> 产物大小"
du -sh "$PKG"
du -sh "$PKG"/lib
echo
echo "把 $PKG 拷到 Windows 的 third_party/opencv-${OPENCV_VER}-armv7/，"
echo "然后 cmake -DMYLIB_ENABLE_CV=ON 即可（Deps.cmake 会校验 ELF 架构）。"
