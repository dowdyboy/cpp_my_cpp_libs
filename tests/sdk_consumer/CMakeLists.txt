cmake_minimum_required(VERSION 3.20)
project(sdk_consumer LANGUAGES CXX)

# =============================================================================
#  SDK 消费方验证工程（不属于主构建，由 CI 与人工单独调用）
#
#  模拟"拿到 my_cpp_libs 安装目录后的第三方工程"：
#     cmake -S tests/sdk_consumer -B build/consumer -G Ninja \
#           -DCMAKE_PREFIX_PATH=/path/to/sdk
#
#  这里刻意**不设置** CMAKE_CXX_STANDARD：公共头要求 C++17，这个要求必须由
#  my_cpp_libsConfig.cmake 通过 INTERFACE_COMPILE_FEATURES 传过来。谁把这条
#  弄丢了，这个工程就会在编译期炸掉 —— 这正是它存在的意义。
# =============================================================================
find_package(my_cpp_libs CONFIG REQUIRED)

add_executable(sdk_consumer main.cpp)
target_link_libraries(sdk_consumer PRIVATE
    my_cpp_libs::core
    my_cpp_libs::io
    my_cpp_libs::time
    my_cpp_libs::thread
    my_cpp_libs::log
    my_cpp_libs::json
    my_cpp_libs::dsp
    my_cpp_libs::net
    my_cpp_libs::serial)
