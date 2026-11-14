# =============================================================================
#  交叉编译工具链：Windows 主机 -> Linux armv7-A（Zynq-7000 / Cortex-A9）
#
#  对应 PetaLinux SDK: D:\ProgramData\gcc-arm-linux-gnueabi
#    bin/arm-linux-gnueabihf-gcc.exe          (GCC 13.3.0)
#    cortexa9t2hf-neon-xilinx-linux-gnueabi/  (目标 sysroot)
#
#  Cortex-A9 要点：
#    -march=armv7-a -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard
#  PetaLinux 用户的 ulimit/内核版本差异不影响交叉编译，但运行时要注意
#  目标板 glibc 版本不低于 SDK 里的版本。
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

set(_mylib_arm_flags "-march=armv7-a -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-pie")
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
