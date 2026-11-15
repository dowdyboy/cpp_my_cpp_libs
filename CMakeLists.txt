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
# 构建身份：git commit
#
# 上板第一件事是打一行 BuildInfo::summary() 确认"跑的到底是哪次构建"。
# 只有版本号而没有 commit，遇到"这版到底修了没有"就说不清。
# git 不在 PATH / 源码不是仓库时降级成 "unknown"，不阻断构建。
# -----------------------------------------------------------------------------
find_package(Git QUIET)
set(MYLIB_GIT_COMMIT "unknown")
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _mylib_git_out
        RESULT_VARIABLE _mylib_git_rc
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_mylib_git_rc EQUAL 0 AND _mylib_git_out)
        set(MYLIB_GIT_COMMIT "${_mylib_git_out}")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" diff-index --quiet HEAD --
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            RESULT_VARIABLE _mylib_git_dirty
            ERROR_QUIET)
        if(NOT _mylib_git_dirty EQUAL 0)
            string(APPEND MYLIB_GIT_COMMIT "-dirty")
        endif()
    endif()
    # commit 变化时自动重新 configure，否则 BuildInfo 里会一直是旧 hash
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD")
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

# -----------------------------------------------------------------------------
# 消毒器（Sanitizer）
#
#    -DMYLIB_SANITIZE=undefined   未定义行为。用 **trap 模式**（UB 编译成 trap 指令），
#                                 不依赖 libubsan，所以 MinGW / 交叉编译也能开，
#                                 代价是只有"过/不过"没有详细报告。
#    -DMYLIB_SANITIZE=address     内存错误。MinGW 不支持 ASan，Linux/WSL 上跑。
#
#   只挂到本项目自己的 target：三方库在 include(Deps) 时已经建好，不受影响。
# -----------------------------------------------------------------------------
add_library(mylib_sanitize INTERFACE)
add_library(my_cpp_libs::sanitize ALIAS mylib_sanitize)

if(MYLIB_SANITIZE)
    if(MSVC)
        if(MYLIB_SANITIZE STREQUAL "address")
            target_compile_options(mylib_sanitize INTERFACE /fsanitize=address /Zi)
            target_link_options(mylib_sanitize INTERFACE /INCREMENTAL:NO)
            message(STATUS "Sanitizer       : address (MSVC)")
        else()
            message(WARNING "MSVC 没有 UBSan，忽略 MYLIB_SANITIZE=${MYLIB_SANITIZE}")
        endif()
    else()
        target_compile_options(mylib_sanitize INTERFACE
            -g -fno-omit-frame-pointer -fno-optimize-sibling-calls)
        if(MYLIB_SANITIZE STREQUAL "address")
            target_compile_options(mylib_sanitize INTERFACE -fsanitize=address)
            target_link_options(mylib_sanitize INTERFACE -fsanitize=address)
            message(STATUS "Sanitizer       : address")
        elseif(MYLIB_SANITIZE STREQUAL "undefined")
            target_compile_options(mylib_sanitize INTERFACE
                -fsanitize=undefined -fsanitize-undefined-trap-on-error)
            target_link_options(mylib_sanitize INTERFACE
                -fsanitize=undefined -fsanitize-undefined-trap-on-error)
            message(STATUS "Sanitizer       : undefined (trap-on-error，不需要 libubsan)")
        else()
            message(WARNING "未知的 MYLIB_SANITIZE=${MYLIB_SANITIZE}，可选：address / undefined")
        endif()
    endif()
endif()

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
#
# 这一屏要能回答"到底编了什么"：组件有没有被静默关掉、推理用的哪个后端、
# 链接的是哪个包。以前只打印 log/json/math/infer，core 和 cv 完全看不见，
# ORT 的包名也不显示 —— armv7 上因此分不清链的是 1.23.2 还是 1.18.1。
# -----------------------------------------------------------------------------
if(TARGET mylib_cv)
    set(_cv_desc "ON")
else()
    set(_cv_desc "OFF")
endif()

if(MYLIB_ENABLE_INFER)
    if(MYLIB_INFER_BACKEND STREQUAL "onnxruntime")
        set(_infer_backend_desc "onnxruntime")
    elseif(MYLIB_HAVE_NCNN)
        set(_infer_backend_desc "ncnn（没有 ONNX Runtime）")
    else()
        set(_infer_backend_desc "stub（没有可用推理库，加载模型会返回 Unsupported）")
    endif()
else()
    set(_infer_backend_desc "off")
endif()

if(MYLIB_ORT_ROOT_DETECTED)
    get_filename_component(_ort_pkg "${MYLIB_ORT_ROOT_DETECTED}" NAME)
else()
    set(_ort_pkg "未接入")
endif()

if(MYLIB_NCNN_ROOT_DETECTED)
    get_filename_component(_ncnn_pkg "${MYLIB_NCNN_ROOT_DETECTED}" NAME)
else()
    set(_ncnn_pkg "未接入")
endif()

if(TARGET mylib_cv)
    set(_cv_pkg "OpenCV ${OpenCV_VERSION}")
else()
    set(_cv_pkg "未接入")
endif()

message(STATUS "")
message(STATUS "================ my_cpp_libs ${PROJECT_VERSION} ================")
message(STATUS "  System          : ${CMAKE_SYSTEM_NAME} / ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  Compiler        : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Cross-compiling : ${CMAKE_CROSSCOMPILING}")
message(STATUS "  Build type      : ${CMAKE_BUILD_TYPE}${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  Git commit      : ${MYLIB_GIT_COMMIT}")
message(STATUS "  Components      : core=${MYLIB_ENABLE_CORE} log=${MYLIB_ENABLE_LOG} json=${MYLIB_ENABLE_JSON} math=${MYLIB_ENABLE_MATH} infer=${MYLIB_ENABLE_INFER} cv=${_cv_desc}")
message(STATUS "  Infer backend   : ${_infer_backend_desc}")
message(STATUS "  ONNX Runtime    : ${_ort_pkg}")
message(STATUS "  ncnn            : ${_ncnn_pkg}")
message(STATUS "  OpenCV          : ${_cv_pkg}")
if(MYLIB_SANITIZE)
    message(STATUS "  Sanitizer       : ${MYLIB_SANITIZE}")
endif()
message(STATUS "  Examples/Tests  : ${MYLIB_BUILD_EXAMPLES}/${MYLIB_BUILD_TESTS}")
message(STATUS "===============================================================")
message(STATUS "")
