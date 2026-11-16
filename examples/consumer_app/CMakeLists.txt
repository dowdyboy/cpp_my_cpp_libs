# =============================================================================
#  独立消费方工程示例（不属于 my_cpp_libs 主构建，单独 configure）
#
#  用法：
#      cmake -S examples/consumer_app -B build/consumer_app -G Ninja
#      cmake --build build/consumer_app
#      ./build/consumer_app/consumer_app
#
#  这就是把 my_cpp_libs 当源码库嵌入自己工程的标准姿势：
#    add_subdirectory(<my_cpp_libs 路径> <二进制目录>)
#    target_link_libraries(app PRIVATE my_cpp_libs::log my_cpp_libs::json ...)
#
#  注意：
#    * 作为子目录时，my_cpp_libs 的示例/测试/工具**默认自动关闭**（看 MYLIB_IS_TOP_LEVEL），
#      不用在这里手工关；
#    * 组件开关用普通 cache 变量设置即可（不要 FORCE，方便命令行覆盖）；
#    * 第一次 configure 会把 resources/*.zip 解压到 my_cpp_libs/third_party，
#      所以离线环境下也要先 `bash scripts/fetch-deps.sh`（或把 zip 放好）。
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(consumer_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 只开自己用得到的组件：这里不需要推理/图像/HTTP/DSP，关掉能显著缩短构建时间。
# 不写这些 set() 也行（默认全开），命令行 -D 覆盖同样有效。
set(MYLIB_ENABLE_INFER OFF CACHE BOOL "")
set(MYLIB_ENABLE_ML    OFF CACHE BOOL "")
set(MYLIB_ENABLE_CV    OFF CACHE BOOL "")
set(MYLIB_ENABLE_HTTP  OFF CACHE BOOL "")
set(MYLIB_ENABLE_IMAGE OFF CACHE BOOL "")
set(MYLIB_ENABLE_DSP   OFF CACHE BOOL "")

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." "${CMAKE_BINARY_DIR}/_my_cpp_libs")

add_executable(consumer_app main.cpp)
target_link_libraries(consumer_app PRIVATE
    my_cpp_libs::log
    my_cpp_libs::json
    my_cpp_libs::math
    my_cpp_libs::net)

# 安装规则：只有在需要的时候才装（本示例不装，避免污染）
# set(MYLIB_INSTALL ON CACHE BOOL "")
