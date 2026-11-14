# =============================================================================
#  my_cpp_libs —— 三方开源库的统一封装层 + 调用演示工程
#
#  设计要点
#   1. 纯源码分发型（source-distributed）：任何目标平台都从源码编译，
#      因此不存在预编译二进制 ABI 不匹配的问题（Zynq / x86 / Windows 通吃）。
#   2. 组件化：core / log / json / math / infer 各自独立成 target，
#      消费方只链接真正需要的组件。
#   3. 三方库通过 resources/*.zip 自动解压挂载，离线可复现。
#
#  用法（消费方）：
#     add_subdirectory(third_party/my_cpp_libs)
#     target_link_libraries(app PRIVATE my_cpp_libs::log my_cpp_libs::json)
# =============================================================================
cmake_minimum_required(VERSION 3.20)

project(my_cpp_libs
    VERSION 0.1.0
    DESCRIPTION "Unified cross-platform C++ wrapper over third-party open-source libraries"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(Platform)
include(Options)
include(Deps)

# 构建期字符串（供 BuildInfo 打印，交叉编译时用于确认产物身份）
if(CMAKE_BUILD_TYPE)
    set(MYLIB_BUILD_TYPE_STR "${CMAKE_BUILD_TYPE}")
elseif(CMAKE_CONFIGURATION_TYPES)
    set(MYLIB_BUILD_TYPE_STR "MultiConfig")
else()
    set(MYLIB_BUILD_TYPE_STR "Unknown")
endif()
if(NOT DEFINED MYLIB_TARGET_TRIPLE)
    set(MYLIB_TARGET_TRIPLE "${CMAKE_SYSTEM_PROCESSOR}-${CMAKE_SYSTEM_NAME}")
endif()

# -----------------------------------------------------------------------------
# 全局编译选项
# -----------------------------------------------------------------------------
# 项目自身使用严格告警；不影响消费方
add_library(mylib_warnings INTERFACE)
if(MSVC)
    target_compile_options(mylib_warnings INTERFACE /W4 /permissive- /Zc:__cplusplus /utf-8)
    target_compile_definitions(mylib_warnings INTERFACE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(mylib_warnings INTERFACE -Wall -Wextra -Wpedantic)
    if(NOT MYLIB_HAS_FMT_FORMAT_STRING) # 仅作占位，见 format.hpp
        # no-op
    endif()
endif()
add_library(my_cpp_libs::warnings ALIAS mylib_warnings)

add_subdirectory(src)

if(MYLIB_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(MYLIB_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(MYLIB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# -----------------------------------------------------------------------------
# 安装
#
# 说明：本库推荐的消费方式是 add_subdirectory（源码分发，换平台就换工具链重编，
# 天然没有 ABI 问题）。因此这里只做最朴素的安装：头文件 + 静态库。
# 带 export set 的 find_package 打包留到 v0.2（需要一并处理 Eigen/nlohmann
# 这类头文件依赖的导出策略）。
# -----------------------------------------------------------------------------
include(GNUInstallDirs)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS ${MYLIB_EFFECTIVE_TARGETS}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# -----------------------------------------------------------------------------
# 配置摘要
# -----------------------------------------------------------------------------
message(STATUS "")
message(STATUS "================ my_cpp_libs ${PROJECT_VERSION} ================")
message(STATUS "  System          : ${CMAKE_SYSTEM_NAME} / ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  Compiler        : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Cross-compiling : ${CMAKE_CROSSCOMPILING}")
message(STATUS "  Build type      : ${CMAKE_BUILD_TYPE}${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  Components      : log=${MYLIB_ENABLE_LOG} json=${MYLIB_ENABLE_JSON} math=${MYLIB_ENABLE_MATH} infer=${MYLIB_ENABLE_INFER}")
message(STATUS "  Infer backend   : ${MYLIB_INFER_BACKEND}")
message(STATUS "  Examples/Tests  : ${MYLIB_BUILD_EXAMPLES}/${MYLIB_BUILD_TESTS}")
message(STATUS "===============================================================")
message(STATUS "")
