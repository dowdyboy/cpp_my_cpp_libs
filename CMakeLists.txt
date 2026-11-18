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
    VERSION 0.1.1
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
        elseif(MYLIB_SANITIZE STREQUAL "thread")
            # TSan：抓并发数据竞争（log 的全局状态、infer 后端注册表、http 的线程管理）
            target_compile_options(mylib_sanitize INTERFACE -fsanitize=thread)
            target_link_options(mylib_sanitize INTERFACE -fsanitize=thread)
            message(STATUS "Sanitizer       : thread (TSan)")
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
# 安装（SDK）
#
# 目标：别的 C++ 工程拿到安装目录后，只要
#     find_package(my_cpp_libs CONFIG REQUIRED)
#     target_link_libraries(app PRIVATE my_cpp_libs::log my_cpp_libs::cv ...)
# 就能用全部组件，不需要自己装 spdlog / Eigen / nlohmann / ORT / OpenCV。
#
# 做法：安装 我们的头 + 静态库 + **公共头会用到的三方头**（json→nlohmann、
# math→Eigen、log→spdlog）以及运行/链接需要的三方库（spdlog 静态库、
# ORT/OpenCV 的 .so/.dll + import lib）；然后生成一份 my_cpp_libsConfig.cmake，
# 在里面把每个组件、每个三方依赖都声明成 IMPORTED target。
#
# 作为子目录被别的工程 add_subdirectory 时，默认**不**添加安装规则。
# -----------------------------------------------------------------------------
if(MYLIB_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(TARGETS ${MYLIB_EFFECTIVE_TARGETS}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    # ---- 公共头需要的三方头 ----
    if(TARGET mylib_json)
        install(DIRECTORY "${MYLIB_DEP_nlohmann-json-3.12.0}/include/nlohmann"
                DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    endif()
    if(TARGET mylib_math)
        install(DIRECTORY "${MYLIB_DEP_eigen-5.0.0}/Eigen"
                DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    endif()
    if(TARGET mylib_log)
        install(DIRECTORY "${MYLIB_DEP_spdlog-1.17.0}/include/spdlog"
                DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
        # spdlog 的编译产物（我们的 log 组件依赖它的符号）
        install(TARGETS spdlog ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
        set(_sdk_spdlog_lib "${CMAKE_STATIC_LIBRARY_PREFIX}spdlog${CMAKE_STATIC_LIBRARY_SUFFIX}")
    else()
        set(_sdk_spdlog_lib "")
    endif()

    # ---- ONNX Runtime（infer/ml 运行时 + 链接）----
    set(_sdk_ort_lib "")
    set(_sdk_ort_dll "")
    if(TARGET deps::onnxruntime AND MYLIB_ORT_ROOT_DETECTED)
        file(GLOB _ort_shared "${MYLIB_ORT_ROOT_DETECTED}/lib/libonnxruntime.so*"
                              "${MYLIB_ORT_ROOT_DETECTED}/lib/onnxruntime.dll")
        foreach(_f ${_ort_shared})
            get_filename_component(_n "${_f}" NAME)
            if(_n MATCHES "\\.dll$")
                install(FILES "${_f}" DESTINATION ${CMAKE_INSTALL_BINDIR})
                set(_sdk_ort_dll "${_n}")
            else()
                install(FILES "${_f}" DESTINATION ${CMAKE_INSTALL_LIBDIR})
            endif()
        endforeach()
        file(GLOB _ort_implib "${MYLIB_ORT_ROOT_DETECTED}/lib/onnxruntime.lib")
        if(_ort_implib)
            install(FILES ${_ort_implib} DESTINATION ${CMAKE_INSTALL_LIBDIR})
            set(_sdk_ort_lib "onnxruntime.lib")
        else()
            # Linux：链接时给具体的 soname 文件
            file(GLOB _ort_so "${MYLIB_ORT_ROOT_DETECTED}/lib/libonnxruntime.so.*")
            if(_ort_so)
                get_filename_component(_n "${_ort_so}" NAME)
                set(_sdk_ort_lib "${_n}")
            endif()
        endif()
    endif()

    # ---- OpenCV（cv 组件的头 + 库）----
    set(_sdk_ocv_include "")
    set(_sdk_ocv_libs "")
    set(_sdk_ocv_dll "")
    if(TARGET deps::opencv)
        # 头目录：优先用 Deps 解析出来的，退回到 OpenCV_INCLUDE_DIRS
        set(_ocv_inc "")
        if(MYLIB_OPENCV_INCLUDE_DIR)
            set(_ocv_inc "${MYLIB_OPENCV_INCLUDE_DIR}")
        elseif(OpenCV_INCLUDE_DIRS)
            list(GET OpenCV_INCLUDE_DIRS 0 _ocv_inc)
        endif()
        if(_ocv_inc AND IS_DIRECTORY "${_ocv_inc}")
            get_filename_component(_incname "${_ocv_inc}" NAME)
            if(_incname STREQUAL "opencv4")
                install(DIRECTORY "${_ocv_inc}/" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/opencv4)
                set(_sdk_ocv_include "${CMAKE_INSTALL_INCLUDEDIR}/opencv4")
            else()
                install(DIRECTORY "${_ocv_inc}/" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
                set(_sdk_ocv_include "${CMAKE_INSTALL_INCLUDEDIR}")
            endif()
        endif()

        if(WIN32 AND DEFINED MYLIB_OPENCV_BIN_DIR)
            file(GLOB _ocv_dlls "${MYLIB_OPENCV_BIN_DIR}/opencv_world[0-9]*.dll")
            foreach(_f ${_ocv_dlls})
                if(NOT _f MATCHES "d\\.dll$")
                    install(FILES "${_f}" DESTINATION ${CMAKE_INSTALL_BINDIR})
                    get_filename_component(_n "${_f}" NAME)
                    set(_sdk_ocv_dll "${_n}")
                    string(REPLACE ".dll" ".lib" _lib "${_n}")
                    if(EXISTS "${MYLIB_OPENCV_BIN_DIR}/../lib/${_lib}")
                        install(FILES "${MYLIB_OPENCV_BIN_DIR}/../lib/${_lib}"
                                DESTINATION ${CMAKE_INSTALL_LIBDIR})
                        set(_sdk_ocv_libs "${_lib}")
                    endif()
                endif()
            endforeach()
        else()
            # Linux：从 OpenCV 安装树里收 .so（真实文件 + SONAME 软链名字都要）
            set(_ocv_libdirs "${MYLIB_OPENCV_LIB_DIR}" "${OpenCV_DIR}" "${OpenCV_DIR}/.." "${OpenCV_DIR}/../..")
            foreach(_d ${_ocv_libdirs})
                if(NOT _d OR NOT IS_DIRECTORY "${_d}")
                    continue()
                endif()
                file(GLOB _ocv_cand "${_d}/libopencv_*.so*")
                if(_ocv_cand)
                    foreach(_f ${_ocv_cand})
                        get_filename_component(_n "${_f}" NAME)
                        if(IS_SYMLINK "${_f}")
                            get_filename_component(_t "${_f}" REALPATH)
                            install(FILES "${_t}" DESTINATION ${CMAKE_INSTALL_LIBDIR} RENAME "${_n}")
                        else()
                            install(FILES "${_f}" DESTINATION ${CMAKE_INSTALL_LIBDIR})
                        endif()
                        list(APPEND _sdk_ocv_libs "${_n}")
                    endforeach()
                    list(REMOVE_DUPLICATES _sdk_ocv_libs)
                    break()
                endif()
            endforeach()
        endif()
    endif()

    # ---- 生成 Config / Version ----
    set(MYLIB_SDK_PLATFORM_DEFS
        "${MYLIB_ARCH_MACRO};MYLIB_PTR_SIZE=${MYLIB_PTR_SIZE};${MYLIB_OS_MACRO}")
    set(MYLIB_SDK_STATIC_PREFIX "${CMAKE_STATIC_LIBRARY_PREFIX}")
    set(MYLIB_SDK_STATIC_SUFFIX "${CMAKE_STATIC_LIBRARY_SUFFIX}")
    set(MYLIB_SDK_SPDLOG_LIB "${_sdk_spdlog_lib}")
    set(MYLIB_SDK_ORT_LIB "${_sdk_ort_lib}")
    set(MYLIB_SDK_ORT_DLL "${_sdk_ort_dll}")
    set(MYLIB_SDK_OCV_INCLUDE "${_sdk_ocv_include}")
    set(MYLIB_SDK_OCV_LIBS "${_sdk_ocv_libs}")
    set(MYLIB_SDK_OCV_DLL "${_sdk_ocv_dll}")

    # 每个组件在 SDK 里要额外链接什么（对应 src/CMakeLists 里的 PUBLIC 依赖）
    set(_mylib_sdk_link_core       "")
    set(_mylib_sdk_link_log        "my_cpp_libs::deps_spdlog")
    set(_mylib_sdk_link_json       "my_cpp_libs::deps_nlohmann")
    set(_mylib_sdk_link_math       "my_cpp_libs::deps_eigen")
    set(_mylib_sdk_link_dsp        "")
    set(_mylib_sdk_link_preprocess "my_cpp_libs::core")
    set(_mylib_sdk_link_net        "")
    set(_mylib_sdk_link_http       "my_cpp_libs::json")
    set(_mylib_sdk_link_infer      "my_cpp_libs::preprocess")
    if(TARGET deps::onnxruntime)
        list(APPEND _mylib_sdk_link_infer my_cpp_libs::deps_onnxruntime)
    endif()
    set(_mylib_sdk_link_ml         "my_cpp_libs::infer;my_cpp_libs::json")
    set(_mylib_sdk_link_image      "my_cpp_libs::preprocess;my_cpp_libs::stb_impl")
    if(TARGET mylib_infer)
        list(APPEND _mylib_sdk_link_image my_cpp_libs::infer)
    endif()
    set(_mylib_sdk_link_cv         "my_cpp_libs::preprocess;my_cpp_libs::deps_opencv")
    if(TARGET mylib_infer)
        list(APPEND _mylib_sdk_link_cv my_cpp_libs::infer)
    endif()

    unset(_sdk_components_code)
    foreach(_c core log json math infer preprocess image dsp ml net http cv)
        if(TARGET mylib_${_c})
            string(APPEND _sdk_components_code
                "_mylib_sdk_add_component(${_c} \"${_mylib_sdk_link_${_c}}\")\n")
        endif()
    endforeach()
    set(MYLIB_SDK_COMPONENTS_BLOCK "${_sdk_components_code}")

    set(_sdk_all_links "")
    foreach(_c core log json math infer preprocess image dsp ml net http cv)
        if(TARGET mylib_${_c})
            list(APPEND _sdk_all_links "my_cpp_libs::${_c}")
        endif()
    endforeach()
    set(MYLIB_SDK_ALL_LINKS "${_sdk_all_links}")

    configure_file(cmake/my_cpp_libsConfig.cmake.in
                   "${CMAKE_CURRENT_BINARY_DIR}/my_cpp_libsConfig.cmake" @ONLY)
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/my_cpp_libsConfigVersion.cmake"
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
    install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/my_cpp_libsConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/my_cpp_libsConfigVersion.cmake"
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/my_cpp_libs)
    message(STATUS "[sdk] find_package 配置: ${CMAKE_INSTALL_LIBDIR}/cmake/my_cpp_libs")
endif()

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

if(TARGET mylib_image)
    set(_image_desc "ON")
else()
    set(_image_desc "OFF")
endif()

if(TARGET mylib_dsp)
    set(_dsp_desc "ON")
else()
    set(_dsp_desc "OFF")
endif()

if(TARGET mylib_ml)
    set(_ml_desc "ON")
else()
    set(_ml_desc "OFF")
endif()

if(TARGET mylib_net)
    set(_net_desc "ON")
else()
    set(_net_desc "OFF")
endif()

if(TARGET mylib_http)
    set(_http_desc "ON")
else()
    set(_http_desc "OFF")
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

# 组件摘要按**实际建出来的 target** 算，不按选项值算。
# 原来 core/log/json/math/infer 打印的是 MYLIB_ENABLE_xxx，于是"选项开着但组件因为
# 缺依赖被跳过"时摘要仍然显示 ON —— 摘要存在的意义就是回答"到底编了什么"。
set(_components_desc "")
foreach(_c core log json math infer image dsp ml net http cv)
    if(TARGET mylib_${_c})
        string(APPEND _components_desc " ${_c}=ON")
    else()
        string(APPEND _components_desc " ${_c}=OFF")
    endif()
endforeach()

message(STATUS "")
message(STATUS "================ my_cpp_libs ${PROJECT_VERSION} ================")
message(STATUS "  System          : ${CMAKE_SYSTEM_NAME} / ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  Compiler        : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Cross-compiling : ${CMAKE_CROSSCOMPILING}")
message(STATUS "  Build type      : ${MYLIB_BUILD_TYPE_STR}")
message(STATUS "  Git commit      : ${MYLIB_GIT_COMMIT}")
message(STATUS "  Components      :${_components_desc}")
message(STATUS "  Infer backend   : ${_infer_backend_desc}")
message(STATUS "  ONNX Runtime    : ${_ort_pkg}")
message(STATUS "  ncnn            : ${_ncnn_pkg}")
message(STATUS "  OpenCV          : ${_cv_pkg}")
if(MYLIB_NO_EXCEPTIONS)
    message(STATUS "  Exceptions      : 关闭（-fno-exceptions，仅 core + math + dsp + net）")
endif()
if(MYLIB_SANITIZE)
    message(STATUS "  Sanitizer       : ${MYLIB_SANITIZE}")
endif()
message(STATUS "  Examples/Tests  : ${MYLIB_BUILD_EXAMPLES}/${MYLIB_BUILD_TESTS}")
message(STATUS "===============================================================")
message(STATUS "")
