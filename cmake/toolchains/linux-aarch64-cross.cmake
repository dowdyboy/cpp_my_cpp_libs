# =============================================================================
#  交叉编译工具链：任意 Linux 主机 -> Linux aarch64
#
#  和 linux-aarch64.cmake 的区别：那个写死了 ARM GNU Toolchain 的绝对路径与
#  libc 子目录布局；这个从 PATH 里找 aarch64-linux-gnu-gcc，供 Linux/CI 使用：
#        sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
# =============================================================================
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(MYLIB_CROSS_PREFIX "aarch64-linux-gnu" CACHE STRING "交叉编译器前缀")

find_program(MYLIB_CROSS_CC  NAMES ${MYLIB_CROSS_PREFIX}-gcc   REQUIRED)
find_program(MYLIB_CROSS_CXX NAMES ${MYLIB_CROSS_PREFIX}-g++   REQUIRED)
set(CMAKE_C_COMPILER   "${MYLIB_CROSS_CC}")
set(CMAKE_CXX_COMPILER "${MYLIB_CROSS_CXX}")

set(_mylib_aarch64_flags "-mcpu=cortex-a53 -fno-pie")
set(CMAKE_C_FLAGS_INIT   "${_mylib_aarch64_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_mylib_aarch64_flags}")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-no-pie")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-no-pie")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# 包搜索用 BOTH：SDK 的 cmake 配置一般装在 sysroot 之外（CI 的 package-linux 就是
# 解包到 /tmp 再用 -DCMAKE_PREFIX_PATH 指过去），设成 ONLY 会让消费方找不到。
# linux-aarch64.cmake / petalinux-2020.2-zynq.cmake 里也是这个口径。
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

set(MYLIB_TARGET_TRIPLE "aarch64-linux-gnu")
