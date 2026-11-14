# =============================================================================
#  PetaLinux 2020.2 SDK 交叉编译工具链（Zynq-7020 / Cortex-A9）
#
#  用途：板子的 rootfs 是 glibc 2.30，和 PC 上 2024 版 SDK（glibc 2.39）不兼容。
#        用这块板子配套的 2020.2 SDK 编译，动态链接产物就能直接在板上跑。
#
#  用法（必须先 source SDK 的环境脚本，把编译器加进 PATH）：
#      source <sdk>/environment-setup-cortexa9t2hf-neon-xilinx-linux-gnueabi
#      cmake -S . -B build/petalinux2020 -G Ninja \
#            -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/petalinux-2020.2-zynq.cmake \
#            -DCMAKE_BUILD_TYPE=Release
#      cmake --build build/petalinux2020
# =============================================================================
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# SDK 根目录：优先用环境变量，否则按默认路径（本项目板子对应的 SDK）
set(MYLIB_PETALINUX_SDK "/home/lfbhlx/Projects/adda_fft/linux/sdk"
    CACHE PATH "PetaLinux 2020.2 SDK 根目录")

if(DEFINED ENV{SDKTARGETSYSROOT})
    set(_sdk_sysroot "$ENV{SDKTARGETSYSROOT}")
else()
    set(_sdk_sysroot "${MYLIB_PETALINUX_SDK}/sysroots/cortexa9t2hf-neon-xilinx-linux-gnueabi")
endif()
message(STATUS "[toolchain] PetaLinux 2020.2 SDK sysroot: ${_sdk_sysroot}")

# 编译器：优先走 PATH（source 过 environment-setup 之后），否则用 SDK 里的绝对路径
find_program(MYLIB_ARM_GCC arm-xilinx-linux-gnueabi-gcc
             HINTS "${MYLIB_PETALINUX_SDK}/sysroots/x86_64-petalinux-linux/usr/bin/arm-xilinx-linux-gnueabi")
find_program(MYLIB_ARM_GXX arm-xilinx-linux-gnueabi-g++
             HINTS "${MYLIB_PETALINUX_SDK}/sysroots/x86_64-petalinux-linux/usr/bin/arm-xilinx-linux-gnueabi")
if(NOT MYLIB_ARM_GCC OR NOT MYLIB_ARM_GXX)
    message(FATAL_ERROR "找不到 arm-xilinx-linux-gnueabi-gcc/g++，请检查 MYLIB_PETALINUX_SDK")
endif()
set(CMAKE_C_COMPILER   "${MYLIB_ARM_GCC}")
set(CMAKE_CXX_COMPILER "${MYLIB_ARM_GXX}")

set(CMAKE_SYSROOT "${_sdk_sysroot}")

# Cortex-A9 + NEON + 硬浮点（和 SDK 里 export 的参数保持一致）
set(_mylib_arm_flags "-mthumb -mfpu=neon -mfloat-abi=hard -mcpu=cortex-a9")
set(CMAKE_C_FLAGS_INIT   "${_mylib_arm_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_mylib_arm_flags}")

set(CMAKE_FIND_ROOT_PATH "${_sdk_sysroot}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(MYLIB_TARGET_TRIPLE "arm-xilinx-linux-gnueabi")

# 注意：这里**不要**设 CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY。
# 那会让 find_package(Threads) 的探测只编译不链接，误判成"libc 自带 pthread"，
# 进而漏掉 -lpthread —— 而 glibc 2.30 的 pthread 还在 libpthread.so 里。
