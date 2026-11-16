# =============================================================================
#  示例：既演示 my_cpp_libs 的封装 API，也演示三方库的原生 API
# =============================================================================
if(NOT TARGET mylib_log AND NOT TARGET mylib_json AND NOT TARGET mylib_math AND NOT TARGET mylib_infer AND NOT TARGET mylib_image)
    message(WARNING "所有组件都被关闭，跳过示例")
    return()
endif()

set(MYLIB_EXAMPLE_BIN_DIR "${CMAKE_BINARY_DIR}/bin")

function(mylib_add_example name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LINK" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE ${ARG_LINK} mylib_warnings mylib_sanitize)
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${MYLIB_EXAMPLE_BIN_DIR}"
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)
    # 多配置生成器（VS）下各配置分目录
    foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
        string(TOUPPER "${cfg}" cfg_upper)
        set_target_properties(${name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${cfg_upper} "${MYLIB_EXAMPLE_BIN_DIR}/${cfg}")
    endforeach()
    # 只有真正链接 infer / cv 的 target 才需要拷 ORT / OpenCV 的 DLL
    if("mylib_infer" IN_LIST ARG_LINK OR "mylib_cv" IN_LIST ARG_LINK)
        mylib_copy_runtime_dlls(${name})
    endif()
endfunction()

# ---------------------------------------------------------------- 封装层示例
# 这些示例都调 my::log 打输出（以及 mix 了别的组件），所以必须同时有 log。
# 只判单个组件的 TARGET 会在"math 开着、log 关着"这类配置下把示例编崩：
# mylib_log 不是 target 时 CMake 会把它当普通库名放过，然后编译期死在
# format.hpp 里的 spdlog/fmt/fmt.h。
if(TARGET mylib_log AND TARGET mylib_core)
    mylib_add_example(example_log SOURCES 01_log_basic/main.cpp
        LINK mylib_log mylib_core)
endif()

if(TARGET mylib_log AND TARGET mylib_json)
    mylib_add_example(example_json SOURCES 02_json_config/main.cpp
        LINK mylib_json mylib_log)
endif()

if(TARGET mylib_log AND TARGET mylib_math)
    mylib_add_example(example_math SOURCES 03_math/main.cpp
        LINK mylib_math mylib_log)
endif()

if(TARGET mylib_log AND TARGET mylib_infer)
    mylib_add_example(example_infer SOURCES 04_infer/main.cpp
        LINK mylib_infer mylib_log)
    mylib_add_example(example_pipeline SOURCES 05_pipeline/main.cpp
        LINK mylib_infer mylib_log)
endif()

if(TARGET mylib_log AND TARGET mylib_cv AND TARGET mylib_infer)
    mylib_add_example(example_opencv SOURCES 06_opencv_pipeline/main.cpp
        LINK mylib_cv mylib_infer mylib_log)
endif()

if(TARGET mylib_log AND TARGET mylib_image AND TARGET mylib_infer)
    # 不依赖 OpenCV 的图像链路：板端（armv7）跑的就是这个。
    # 示例本身要 create_session()，所以还要 infer；image 组件自己不需要它。
    mylib_add_example(example_image SOURCES 07_image_pipeline/main.cpp
        LINK mylib_image mylib_infer mylib_log)
endif()

if(TARGET mylib_log AND TARGET mylib_dsp)
    # 信号全链路：频谱 -> 检测 -> 识别 -> 解调（信号是合成的，不接硬件）
    mylib_add_example(example_signal SOURCES 08_signal_pipeline/main.cpp
        LINK mylib_dsp mylib_log)
endif()

if(TARGET mylib_log AND TARGET mylib_net)
    # TCP 回显 + UDP 往返 + 长度前缀分帧（回环，不需要外部网络）
    mylib_add_example(example_net SOURCES 10_net_demo/main.cpp
        LINK mylib_net mylib_log)
endif()

if(TARGET mylib_log AND TARGET mylib_http)
    # 板端 REST：/status + /echo；客户端顺手打一遍（也可以 -D 指向自己的 Flask）
    mylib_add_example(example_http SOURCES 11_http_api/main.cpp
        LINK mylib_http mylib_json mylib_log)
endif()

if(TARGET mylib_log AND TARGET mylib_ml)
    # sklearn 模型（models/ml/ 下）的 C++ 侧用法：单样本 + 批量
    mylib_add_example(example_ml SOURCES 09_ml_iris/main.cpp
        LINK mylib_ml mylib_log)
endif()

# ---------------------------------------------------------------- 原生 API 示例
# 直接对着三方库编程，用来对照"封装前"的写法
if(TARGET deps::spdlog)
    mylib_add_example(example_native_spdlog SOURCES 10_native_spdlog/main.cpp
        LINK deps::spdlog)
endif()

if(TARGET deps::json)
    mylib_add_example(example_native_json SOURCES 11_native_nlohmann/main.cpp
        LINK deps::json)
endif()

if(TARGET deps::eigen)
    mylib_add_example(example_native_eigen SOURCES 12_native_eigen/main.cpp
        LINK deps::eigen)
endif()

if(TARGET deps::onnxruntime)
    mylib_add_example(example_native_onnxruntime SOURCES 13_native_onnxruntime/main.cpp
        LINK deps::onnxruntime)
endif()

if(TARGET deps::ncnn)
    mylib_add_example(example_native_ncnn SOURCES 14_native_ncnn/main.cpp
        LINK deps::ncnn)
endif()
