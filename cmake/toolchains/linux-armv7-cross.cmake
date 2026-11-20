# =============================================================================
#  交叉编译工具链：任意 Linux 主机 -> Linux armv7-A 硬浮点（Cortex-A9）
#
#  和 linux-armv7-zynq.cmake 的区别：
#    * 那个指向 Windows 上某台机器的 PetaLinux SDK（绝对路径，只在那台机器有效）；
#    * 这个从 PATH 里找 arm-linux-gnueabihf-gcc，供 Linux/CI 使用：
#        Debian/Ubuntu:  sudo apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf
#
#  注意：apt 工具链的 sysroot glibc 通常比 PetaLinux 2020.2（glibc 2.30）新，
#  产物只能做**编译/链接验证**，不能直接拿到老板子上跑。要上板请用
#  petalinux-2020.2-zynq.cmake 或加 -DCMAKE_EXE_LINKER_FLAGS=-static。
# =============================================================================
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(MYLIB_CROSS_PREFIX "arm-linux-gnueabihf" CACHE STRING "交叉编译器前缀")

find_program(MYLIB_CROSS_CC  NAMES ${MYLIB_CROSS_PREFIX}-gcc   REQUIRED)
find_program(MYLIB_CROSS_CXX NAMES ${MYLIB_CROSS_PREFIX}-g++   REQUIRED)
set(CMAKE_C_COMPILER   "${MYLIB_CROSS_CC}")
set(CMAKE_CXX_COMPILER "${MYLIB_CROSS_CXX}")

# 只给 -mcpu：-mcpu=cortex-a9 已经隐含 armv7-a + NEON，
# 再同时写 -march=armv7-a 会触发 "conflicts with -march=armv7-a+simd" 告警（每个 TU 一条）。
set(_mylib_arm_flags "-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-pie")
set(CMAKE_C_FLAGS_INIT   "${_mylib_arm_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_mylib_arm_flags}")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-no-pie")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-no-pie")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# 包搜索用 BOTH：SDK 的 cmake 配置一般装在 sysroot 之外（CI 的 package-linux 就是
# 解包到 /tmp 再用 -DCMAKE_PREFIX_PATH 指过去），设成 ONLY 会让消费方找不到。
# linux-aarch64.cmake / petalinux-2020.2-zynq.cmake 里也是这个口径。
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

set(MYLIB_TARGET_TRIPLE "arm-linux-gnueabihf")
