# =============================================================================
#  平台 / 编译器探测与公共编译设置
# =============================================================================
find_package(Threads REQUIRED)

# ---------------------------------------------------------------- 架构与平台宏
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(MYLIB_ARCH "x86_64")
    set(MYLIB_ARCH_MACRO MYLIB_ARCH_X86_64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(MYLIB_ARCH "aarch64")
    set(MYLIB_ARCH_MACRO MYLIB_ARCH_AARCH64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(i.86|x86)$")
    set(MYLIB_ARCH "x86")
    set(MYLIB_ARCH_MACRO MYLIB_ARCH_X86)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
    set(MYLIB_ARCH "arm")
    set(MYLIB_ARCH_MACRO MYLIB_ARCH_ARM)
else()
    set(MYLIB_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    set(MYLIB_ARCH_MACRO MYLIB_ARCH_UNKNOWN)
endif()

# 目标系统的字长 —— 交叉编译时不能看 CMAKE_SIZEOF_VOID_P 之外的信息
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(MYLIB_PTR_SIZE 64)
else()
    set(MYLIB_PTR_SIZE 32)
endif()

# ---------------------------------------------------------------- 平台 interface 库
add_library(mylib_platform INTERFACE)
add_library(my_cpp_libs::platform ALIAS mylib_platform)

target_compile_features(mylib_platform INTERFACE cxx_std_17)
target_compile_definitions(mylib_platform INTERFACE
    ${MYLIB_ARCH_MACRO}
    MYLIB_PTR_SIZE=${MYLIB_PTR_SIZE})
# 公共头里有中文字符串字面量：MSVC 必须 /utf-8，否则消费方按 ANSI 读会报
# "常量中有换行符"。这些选项要 PUBLIC，否则 add_subdirectory 的宿主工程也会中招。
if(MSVC)
    target_compile_options(mylib_platform INTERFACE /utf-8 /Zc:__cplusplus)
    target_compile_definitions(mylib_platform INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(WIN32)
    target_compile_definitions(mylib_platform INTERFACE MYLIB_OS_WINDOWS)
    set(MYLIB_OS_MACRO MYLIB_OS_WINDOWS)
elseif(APPLE)
    target_compile_definitions(mylib_platform INTERFACE MYLIB_OS_MACOS)
    set(MYLIB_OS_MACRO MYLIB_OS_MACOS)
elseif(UNIX)
    target_compile_definitions(mylib_platform INTERFACE MYLIB_OS_LINUX)
    set(MYLIB_OS_MACRO MYLIB_OS_LINUX)
endif()

target_link_libraries(mylib_platform INTERFACE Threads::Threads)

if(MYLIB_BUILD_SHARED)
    target_compile_definitions(mylib_platform INTERFACE MYLIB_SHARED)
endif()

# ---------------------------------------------------------------- 便捷函数
# mylib_add_component(<name> [SOURCES ...] [LINK ...] [DEPS ...])
#   - 生成 target `mylib_<name>` 与别名 `my_cpp_libs::<name>`
#   - 统一挂上 platform / warnings / 导出宏
function(mylib_add_component name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LINK;BUILD_DEPS" ${ARGN})
    add_library(mylib_${name} ${_mylib_lib_type} ${ARG_SOURCES})
    add_library(my_cpp_libs::${name} ALIAS mylib_${name})

    set_target_properties(mylib_${name} PROPERTIES
        OUTPUT_NAME "my_cpp_libs_${name}"
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON
        EXPORT_NAME ${name})

    target_include_directories(mylib_${name}
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
               $<INSTALL_INTERFACE:include>)

    target_link_libraries(mylib_${name} PUBLIC mylib_platform ${ARG_LINK})
    target_link_libraries(mylib_${name} PRIVATE mylib_warnings)
    # -fno-exceptions 之类的编译标志只在"本库 target"上生效：
    # PRIVATE 链接 => 编译本组件时用，但不传染给消费方（消费方可能开着异常）。
    if(TARGET mylib_noexcept)
        target_link_libraries(mylib_${name} PRIVATE mylib_noexcept)
    endif()
    if(TARGET mylib_sanitize)
        # PUBLIC：消毒器的链接选项必须出现在最终可执行文件的链接行上
        target_link_libraries(mylib_${name} PUBLIC mylib_sanitize)
    endif()
    if(ARG_BUILD_DEPS)
        add_dependencies(mylib_${name} ${ARG_BUILD_DEPS})
    endif()
    if(MYLIB_BUILD_SHARED)
        target_compile_definitions(mylib_${name} PRIVATE MYLIB_BUILDING MYLIB_SHARED)
        set_target_properties(mylib_${name} PROPERTIES
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON)
    endif()

    # 汇编/编译告警在 Debug 下不阻断（模板库实例化噪音较多）
    if(MSVC)
        target_compile_options(mylib_${name} PRIVATE /bigobj)
    endif()
endfunction()

# ---------------------------------------------------------------- 运行时 DLL
# Windows 上把**这个 target 真正需要的**运行时 DLL 拷到它旁边，省得手动配 PATH。
# 只处理 ONNX Runtime 和 OpenCV 两个会带 DLL 的依赖；调用方应先判断
# target 是否真的链接了 mylib_infer / mylib_cv，否则会把无谓的复制挂到每个 target 上。
function(mylib_copy_runtime_dlls target)
    if(NOT WIN32)
        return()
    endif()

    set(_dlls "")
    # 优先用自动探测到的 ORT 根目录（MYLIB_ONNXRUNTIME_ROOT 是"用户显式指定"的入口，
    # 自动探测结果不再回写它，见 cmake/Deps.cmake）
    if(DEFINED MYLIB_ORT_ROOT_DETECTED AND MYLIB_ORT_ROOT_DETECTED)
        if(EXISTS "${MYLIB_ORT_ROOT_DETECTED}/lib/onnxruntime.dll")
            list(APPEND _dlls "${MYLIB_ORT_ROOT_DETECTED}/lib/onnxruntime.dll")
        endif()
    endif()
    if(DEFINED MYLIB_OPENCV_RUNTIME_DLLS)
        list(APPEND _dlls ${MYLIB_OPENCV_RUNTIME_DLLS})
    endif()

    foreach(_dll ${_dlls})
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" "$<TARGET_FILE_DIR:${target}>/"
            COMMENT "复制 ${_dll}")
    endforeach()
endfunction()
