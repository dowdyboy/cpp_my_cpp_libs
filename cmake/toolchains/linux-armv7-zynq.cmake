# =============================================================================
#  交叉编译工具链：Windows 主机 -> Linux armv7-A（Cortex-A9）
#
#  对应 SDK: D:\ProgramData\gcc-arm-linux-gnueabi   （**2024 版**，GCC 13.3）
#    bin/arm-linux-gnueabihf-gcc.exe
#    cortexa9t2hf-neon-xilinx-linux-gnueabi/      （目标 sysroot，glibc 2.39）
#
#  ⚠ 这个 sysroot 的 glibc 是 2.39。PetaLinux 2020.2 的板子只有 glibc 2.30，
#    动态链接产物上板会报 GLIBC_2.38 not found。所以本工具链的定位是
#    "编译/链接验证"，要上板请二选一：
#      * cmake --preset linux-armv7-static     （全静态，板上不依赖任何库）
#      * cmake --preset linux-armv7-sdk2020    （用板子配套的 2020.2 SDK）
#
#  Cortex-A9 要点：-mcpu=cortex-a9 已经隐含 armv7-a + NEON，
#  不要再同时写 -march=armv7-a —— GCC 会把它规范化成 armv7-a+simd，
#  然后每个编译单元报一条 "conflicts with -march" 的告警。
# =============================================================================
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(MYLIB_TOOLCHAIN_ROOT "D:/ProgramData/gcc-arm-linux-gnueabi"
    CACHE PATH "PetaLinux SDK 根目录")
set(MYLIB_TARGET_SYSROOT
    "${MYLIB_TOOLCHAIN_ROOT}/cortexa9t2hf-neon-xilinx-linux-gnueabi"
    CACHE PATH "目标 sysroot")

set(CMAKE_C_COMPILER   "${MYLIB_TOOLCHAIN_ROOT}/bin/arm-linux-gnueabihf-gcc.exe")
set(CMAKE_CXX_COMPILER "${MYLIB_TOOLCHAIN_ROOT}/bin/arm-linux-gnueabihf-g++.exe")

set(CMAKE_SYSROOT "${MYLIB_TARGET_SYSROOT}")

set(_mylib_arm_flags "-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-pie")
set(CMAKE_C_FLAGS_INIT   "${_mylib_arm_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_mylib_arm_flags}")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-no-pie")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-no-pie")

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(MYLIB_TARGET_TRIPLE "arm-linux-gnueabihf")
