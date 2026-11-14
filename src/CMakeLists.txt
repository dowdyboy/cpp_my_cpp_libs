# =============================================================================
#  组件源文件
# =============================================================================
set(MYLIB_TARGETS "")

# ---------------------------------------------------------------- core
# 构建信息头（用 configure_file 生成，避免跨生成器的字符串转义问题）
if(CMAKE_CROSSCOMPILING)
    set(MYLIB_CROSS_COMPILED_INT 1)
else()
    set(MYLIB_CROSS_COMPILED_INT 0)
endif()
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/core/build_info_config.h.in"
    "${CMAKE_BINARY_DIR}/generated/my_cpp_libs/build_info_config.h"
    @ONLY)

set(_core_sources core/status.cpp core/build_info.cpp)
mylib_add_component(core SOURCES ${_core_sources})
target_include_directories(mylib_core PRIVATE "${CMAKE_BINARY_DIR}/generated")
list(APPEND MYLIB_TARGETS mylib_core)

# ---------------------------------------------------------------- log
if(MYLIB_ENABLE_LOG)
    mylib_add_component(log
        SOURCES log/log.cpp
        LINK deps::spdlog)
    list(APPEND MYLIB_TARGETS mylib_log)
endif()

# ---------------------------------------------------------------- json
if(MYLIB_ENABLE_JSON)
    mylib_add_component(json
        SOURCES json/json.cpp
        LINK mylib_core deps::json)
    list(APPEND MYLIB_TARGETS mylib_json)
endif()

# ---------------------------------------------------------------- math
if(MYLIB_ENABLE_MATH)
    mylib_add_component(math
        SOURCES math/math.cpp
        LINK mylib_core deps::eigen)
    list(APPEND MYLIB_TARGETS mylib_math)
endif()

# ---------------------------------------------------------------- infer
if(MYLIB_ENABLE_INFER)
    set(_infer_sources infer/infer.cpp infer/backend_stub.cpp infer/preprocess.cpp)
    set(_infer_link mylib_core)
    if(MYLIB_INFER_BACKEND STREQUAL "onnxruntime")
        list(APPEND _infer_sources infer/backend_onnxruntime.cpp)
        list(APPEND _infer_link deps::onnxruntime)
    endif()
    if(MYLIB_HAVE_NCNN)
        list(APPEND _infer_sources infer/backend_ncnn.cpp)
        list(APPEND _infer_link deps::ncnn)
    endif()
    mylib_add_component(infer SOURCES ${_infer_sources} LINK ${_infer_link})
    list(APPEND MYLIB_TARGETS mylib_infer)
endif()

# ---------------------------------------------------------------- cv（OpenCV）
if(MYLIB_ENABLE_CV AND MYLIB_HAVE_CV)
    mylib_add_component(cv
        SOURCES cv/image.cpp
        LINK mylib_core mylib_infer deps::opencv)
    # 让汇总头 my_cpp_libs.hpp 知道可以包含 cv 组件
    target_compile_definitions(mylib_cv PUBLIC MYLIB_ENABLE_CV_COMPONENT=1)
    list(APPEND MYLIB_TARGETS mylib_cv)
endif()

# ---------------------------------------------------------------- 聚合 target
add_library(mylib_all INTERFACE)
add_library(my_cpp_libs::all ALIAS mylib_all)
target_link_libraries(mylib_all INTERFACE ${MYLIB_TARGETS})
target_include_directories(mylib_all INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<INSTALL_INTERFACE:include>)

set(MYLIB_EFFECTIVE_TARGETS "${MYLIB_TARGETS}" PARENT_SCOPE)
