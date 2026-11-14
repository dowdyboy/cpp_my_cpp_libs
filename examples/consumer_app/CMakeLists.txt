# =============================================================================
#  独立消费方工程示例（不属于 my_cpp_libs 主构建，单独 configure）
#
#  用法：
#      cmake -S examples/consumer_app -B build/consumer_app -G Ninja
#      cmake --build build/consumer_app
#
#  这就是把 my_cpp_libs 当源码库嵌入自己工程的标准姿势：
#    add_subdirectory(<my_cpp_libs 路径> <二进制目录>)
#    target_link_libraries(app PRIVATE my_cpp_libs::log my_cpp_libs::json)
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(consumer_app LANGUAGES CXX)

# 只产出要用的组件，不构建示例/测试
set(MYLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MYLIB_BUILD_TESTS    OFF CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." "${CMAKE_BINARY_DIR}/_my_cpp_libs")

add_executable(consumer_app main.cpp)
target_link_libraries(consumer_app PRIVATE my_cpp_libs::log my_cpp_libs::json)
