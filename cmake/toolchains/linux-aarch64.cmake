# =============================================================================
#  交叉编译工具链：Windows 主机 -> Linux aarch64（ZynqMP / RK3588 等）
#
#  对应 ARM GNU Toolchain 14.2:
#    D:\ProgramData\arm-gnu-toolchain-14.2.rel1-mingw-w64-x86_64-aarch64-none-linux-gnu
#
#  两个坑（已在此处理）：
#    1. ARM GNU Toolchain 把 glibc sysroot 放在 <prefix>/libc，而不是 <prefix>；
#       指向 <prefix> 会报 "cannot find crt1.o"。
#    2. <prefix>/lib 里是 libstdc++ 等运行库，不在 sysroot 中，由编译器内置路径覆盖。
#
#  若目标板系统比工具链自带的 glibc 旧，请把 MYLIB_TARGET_SYSROOT 指向板子 SDK 的
#  sysroot（用 -DMYLIB_TARGET_SYSROOT=... 覆盖）。
# =============================================================================
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(MYLIB_TOOLCHAIN_ROOT
    "D:/ProgramData/arm-gnu-toolchain-14.2.rel1-mingw-w64-x86_64-aarch64-none-linux-gnu"
    CACHE PATH "ARM GNU Toolchain 根目录")
set(MYLIB_TARGET_SYSROOT "${MYLIB_TOOLCHAIN_ROOT}/aarch64-none-linux-gnu/libc"
    CACHE PATH "目标 sysroot（glibc）")

set(CMAKE_C_COMPILER   "${MYLIB_TOOLCHAIN_ROOT}/bin/aarch64-none-linux-gnu-gcc.exe")
set(CMAKE_CXX_COMPILER "${MYLIB_TOOLCHAIN_ROOT}/bin/aarch64-none-linux-gnu-g++.exe")

set(CMAKE_SYSROOT "${MYLIB_TARGET_SYSROOT}")

set(CMAKE_C_FLAGS_INIT   "-march=armv8-a -fno-pie")
set(CMAKE_CXX_FLAGS_INIT "-march=armv8-a -fno-pie")
# -no-pie 只给可执行文件。加到共享库上会让链接器去用 crt1.o 找 _start/main，
# 报 "undefined reference to `main`"（交叉编 OpenCV 的 .so 时踩过）。
set(CMAKE_EXE_LINKER_FLAGS_INIT "-no-pie")

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# 包搜索允许 BOTH：SDK（my_cpp_libs/ORT/OpenCV 的 cmake 配置）通常装在 sysroot 之外，
# 设成 ONLY 会让消费方 find_package(my_cpp_libs) 直接找不到。
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

set(MYLIB_TARGET_TRIPLE "aarch64-none-linux-gnu")
