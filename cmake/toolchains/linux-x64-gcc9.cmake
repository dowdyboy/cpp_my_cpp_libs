# =============================================================================
#  交叉编译工具链：Windows 主机 -> Linux x86_64
#
#  对应 D:\ProgramData\x86_64-linux-gnu\gcc-9.3.0（Ubuntu 20.04 / glibc 2.31）
#  注意：
#    1. 同时安装了 Eclipse 与 MSVC 两种 ABI 的产物，这里用 mingw 宿主版
#    2. 该 sysroot 的默认 PIE/栈保护设置与宿主 gcc 不同，
#       必须加 -mstack-protector-guard=tls 和 -fno-pie/-no-pie，否则链接报
#       undefined reference to `__stack_chk_guard'
# =============================================================================
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MYLIB_TOOLCHAIN_ROOT "D:/ProgramData/x86_64-linux-gnu/gcc-9.3.0"
    CACHE PATH "交叉工具链根目录")

set(CMAKE_C_COMPILER   "${MYLIB_TOOLCHAIN_ROOT}/bin/x86_64-linux-gnu-gcc.exe")
set(CMAKE_CXX_COMPILER "${MYLIB_TOOLCHAIN_ROOT}/bin/x86_64-linux-gnu-g++.exe")

set(CMAKE_SYSROOT "${MYLIB_TOOLCHAIN_ROOT}/x86_64-linux-gnu/sysroot")

set(_mylib_common_flags "-mstack-protector-guard=tls -fno-pie")
set(CMAKE_C_FLAGS_INIT      "${_mylib_common_flags}")
set(CMAKE_CXX_FLAGS_INIT    "${_mylib_common_flags}")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-no-pie")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-no-pie")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-no-pie")

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(MYLIB_TARGET_TRIPLE "x86_64-linux-gnu")
