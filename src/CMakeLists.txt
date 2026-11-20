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
# 本构建是否开异常：BuildInfo::hasExceptions() 用它，测试据此跳过
# "回调抛异常要被吞掉"这类只有异常开启时才有意义的用例
if(MYLIB_NO_EXCEPTIONS)
    set(MYLIB_HAS_EXCEPTIONS_INT 0)
else()
    set(MYLIB_HAS_EXCEPTIONS_INT 1)
endif()
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/core/build_info_config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/my_cpp_libs/build_info_config.h"
    @ONLY)

set(_core_sources core/status.cpp core/build_info.cpp core/hash.cpp)
mylib_add_component(core SOURCES ${_core_sources})
target_include_directories(mylib_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
list(APPEND MYLIB_TARGETS mylib_core)

# ---------------------------------------------------------------- io
# 文件/目录/路径/临时文件。全部走 std::filesystem 的 error_code 重载，不抛异常，
# 所以能进 -fno-exceptions 子集。write_file 是原子的（临时文件 + rename）。
if(MYLIB_ENABLE_IO)
    mylib_add_component(io
        SOURCES io/io.cpp
        LINK mylib_core)
    target_compile_definitions(mylib_io PUBLIC MYLIB_ENABLE_IO_COMPONENT=1)
    list(APPEND MYLIB_TARGETS mylib_io)
endif()

# ---------------------------------------------------------------- time
# 单调时钟 / 时间戳格式化 / 计时器。只有标准库依赖。
if(MYLIB_ENABLE_TIME)
    mylib_add_component(time
        SOURCES time/time.cpp
        LINK mylib_core)
    target_compile_definitions(mylib_time PUBLIC MYLIB_ENABLE_TIME_COMPONENT=1)
    list(APPEND MYLIB_TARGETS mylib_time)
endif()

# ---------------------------------------------------------------- thread
# 线程池 / 阻塞队列 / 周期任务。Threads::Threads 由 mylib_platform 统一带下来。
if(MYLIB_ENABLE_THREAD)
    mylib_add_component(thread
        SOURCES thread/thread.cpp
        LINK mylib_core)
    target_compile_definitions(mylib_thread PUBLIC MYLIB_ENABLE_THREAD_COMPONENT=1)
    list(APPEND MYLIB_TARGETS mylib_thread)
endif()

# ---------------------------------------------------------------- log
if(MYLIB_ENABLE_LOG)
    mylib_add_component(log
        SOURCES log/log.cpp
        LINK deps::spdlog)
    target_compile_definitions(mylib_log PUBLIC MYLIB_ENABLE_LOG_COMPONENT=1)
        list(APPEND MYLIB_TARGETS mylib_log)
endif()

# ---------------------------------------------------------------- json
if(MYLIB_ENABLE_JSON)
    mylib_add_component(json
        SOURCES json/json.cpp
        LINK mylib_core deps::json)
    target_compile_definitions(mylib_json PUBLIC MYLIB_ENABLE_JSON_COMPONENT=1)
        list(APPEND MYLIB_TARGETS mylib_json)
endif()

# ---------------------------------------------------------------- math
if(MYLIB_ENABLE_MATH)
    mylib_add_component(math
        SOURCES math/math.cpp
        LINK mylib_core deps::eigen)
    target_compile_definitions(mylib_math PUBLIC MYLIB_ENABLE_MATH_COMPONENT=1)
        list(APPEND MYLIB_TARGETS mylib_math)
endif()

# ---------------------------------------------------------------- dsp（信号处理）
# 频谱 / 滤波 / 检测 / 识别 / 解调。只依赖 core + pocketfft（单头文件），
# 和推理、图像都没有关系，板端可以单独编进去。
if(MYLIB_ENABLE_DSP)
    mylib_add_component(dsp
        SOURCES dsp/spectrum.cpp dsp/filter.cpp dsp/stream.cpp dsp/analyze.cpp dsp/demod.cpp
        LINK mylib_core deps::pocketfft)
    target_compile_definitions(mylib_dsp PUBLIC MYLIB_ENABLE_DSP_COMPONENT=1)
    list(APPEND MYLIB_TARGETS mylib_dsp)
endif()

# ---------------------------------------------------------------- preprocess
# 纯数组的预处理/后处理（归一化、letterbox、softmax/topk、NMS）。
#
# 单独成 target 的理由：它被 infer / image / cv 三个组件共用，但自己
# **不依赖任何三方库**。以前它编在 mylib_infer 里，于是"要用 image 就
# 得把整个 ORT 拖进来"。拆出来之后：
#   * 板端只开 image 也能用 letterbox 和归一化；
#   * cv 不再需要链 infer（以前 -DMYLIB_ENABLE_INFER=OFF 会直接 configure 失败）。
# 公共头仍在 include/my_cpp_libs/infer/preprocess.hpp，不破坏已有 include。
if(MYLIB_ENABLE_INFER OR MYLIB_ENABLE_IMAGE OR MYLIB_ENABLE_CV)
    mylib_add_component(preprocess
        SOURCES infer/preprocess.cpp
        LINK mylib_core)
    list(APPEND MYLIB_TARGETS mylib_preprocess)
endif()

# ---------------------------------------------------------------- net（TCP/UDP）
# 裸 socket 的跨平台薄封装：WSAStartup / SOCKET vs int / SIGPIPE / 超时 / 广播组播
# 全在这一个组件里吃掉，不引 Asio 也不引 libuv。
set(_net_link mylib_core)
if(WIN32)
    list(APPEND _net_link ws2_32)
endif()
if(MYLIB_ENABLE_NET)
    mylib_add_component(net
        SOURCES net/net.cpp
        LINK ${_net_link})
    target_compile_definitions(mylib_net PUBLIC MYLIB_ENABLE_NET_COMPONENT=1)
    list(APPEND MYLIB_TARGETS mylib_net)
endif()

# ---------------------------------------------------------------- serial（串口）
# Windows COM（DCB + COMMTIMEOUTS + ClearCommError）与 Linux termios（poll + FIONREAD）
# 的薄封装。只用系统调用 + Status，没有 try/catch，所以也在 -fno-exceptions 子集里。
if(MYLIB_ENABLE_SERIAL)
    mylib_add_component(serial
        SOURCES serial/serial.cpp
        LINK mylib_core)
    target_compile_definitions(mylib_serial PUBLIC MYLIB_ENABLE_SERIAL_COMPONENT=1)
    list(APPEND MYLIB_TARGETS mylib_serial)
endif()
# ---------------------------------------------------------------- http（客户端 + 服务端）
# 依赖 cpp-httplib（单头文件）+ json（JSON 便捷接口）。公共头不暴露 httplib。
if(MYLIB_ENABLE_HTTP)
    if(NOT TARGET deps::httplib)
        message(WARNING "MYLIB_ENABLE_HTTP=ON 但找不到 cpp-httplib —— http 组件跳过")
    elseif(NOT TARGET mylib_json)
        message(WARNING "MYLIB_ENABLE_HTTP=ON 但 json 组件被关掉了 —— http 组件跳过（JSON 便捷接口需要它）")
    else()
        # httplib 只出现在 src/http/http.cpp 一个 TU 里：用 PRIVATE 链接，
        # 这样它带的 NOMINMAX / WIN32_LEAN_AND_MEAN / _WIN32_WINNT 不会传播给消费方
        # （ws2_32 仍要 PUBLIC，静态库下游链接时需要它）。
        set(_http_link mylib_core mylib_json)
        if(WIN32)
            list(APPEND _http_link ws2_32)
        endif()
        mylib_add_component(http
            SOURCES http/http.cpp
            LINK ${_http_link})
        target_link_libraries(mylib_http PRIVATE deps::httplib)
        target_compile_definitions(mylib_http PUBLIC MYLIB_ENABLE_HTTP_COMPONENT=1)
        list(APPEND MYLIB_TARGETS mylib_http)
    endif()
endif()

# ---------------------------------------------------------------- infer
if(MYLIB_ENABLE_INFER)
    set(_infer_sources infer/infer.cpp infer/backend_stub.cpp)
    set(_infer_link mylib_core mylib_preprocess)
    if(MYLIB_INFER_BACKEND STREQUAL "onnxruntime")
        list(APPEND _infer_sources infer/backend_onnxruntime.cpp)
        list(APPEND _infer_link deps::onnxruntime)
    endif()
    if(MYLIB_HAVE_NCNN)
        list(APPEND _infer_sources infer/backend_ncnn.cpp)
        list(APPEND _infer_link deps::ncnn)
    endif()
    mylib_add_component(infer SOURCES ${_infer_sources} LINK ${_infer_link})
    target_compile_definitions(mylib_infer PUBLIC MYLIB_ENABLE_INFER_COMPONENT=1)
        list(APPEND MYLIB_TARGETS mylib_infer)
endif()

# ---------------------------------------------------------------- ml（机器学习）
# sklearn 等框架导出的表格模型推理。依赖三样：
#   * infer —— 走 ONNX Runtime 后端加载 .onnx；
#   * json  —— 读同名的 .json sidecar（特征名/类别名）；没有 json 组件也能编，
#              只是拿不到元数据（特征名/类别名），运行时要求输入维度是静态的。
# 不引入新的三方库：ONNX 用现成的 ORT，元数据用现成的 nlohmann/json。
if(MYLIB_ENABLE_ML)
    if(NOT TARGET mylib_infer)
        message(WARNING "MYLIB_ENABLE_ML=ON 但 infer 组件被关掉了 —— ml 组件跳过")
    else()
        set(_ml_link mylib_core mylib_infer)
        if(TARGET mylib_json)
            list(APPEND _ml_link mylib_json)
        endif()
        mylib_add_component(ml SOURCES ml/ml.cpp LINK ${_ml_link})
        if(TARGET mylib_json)
            target_compile_definitions(mylib_ml PRIVATE MYLIB_ML_HAS_JSON=1)
        else()
            message(WARNING
                "MYLIB_ENABLE_ML=ON 但 json 组件被关掉了 —— ml 能编、能推理，\n"
                "  但读不了 sidecar（特征名/类别名），且要求模型输入是静态维度")
        endif()
        target_compile_definitions(mylib_ml PUBLIC MYLIB_ENABLE_ML_COMPONENT=1)
        list(APPEND MYLIB_TARGETS mylib_ml)
    endif()
endif()

# ---------------------------------------------------------------- image（stb_image）
# 板端用的图像链路：解码 -> resize/letterbox -> 归一化 -> 张量。
# 与 cv 组件的区别：**不依赖 OpenCV**，所以 armv7 上也能编、也能跑。
if(MYLIB_ENABLE_IMAGE)
    # stb 的实现单独成一个 target：它只需要 include 路径，
    # 不该继承 -Wall -Wextra -Wpedantic，也不该把 -w 传染给别的target。
    add_library(mylib_stb_impl STATIC image/stb_impl.cpp)
    target_link_libraries(mylib_stb_impl PRIVATE deps::stb)
    set_target_properties(mylib_stb_impl PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(mylib_stb_impl PRIVATE /w)
    else()
        target_compile_options(mylib_stb_impl PRIVATE -w)
    endif()

    set(_image_link mylib_core mylib_preprocess deps::stb mylib_stb_impl)
    # Shape 的成员函数（elements/to_string）定义在 infer 里；有就链上，
    # 没有也不影响 image 自己工作（只用到聚合初始化）。
    if(TARGET mylib_infer)
        list(APPEND _image_link mylib_infer)
    endif()
    mylib_add_component(image
        SOURCES image/image.cpp
        LINK ${_image_link})
    # 让汇总头 my_cpp_libs.hpp 知道可以包含 image 组件
    target_compile_definitions(mylib_image PUBLIC MYLIB_ENABLE_IMAGE_COMPONENT=1)
    # stb 的实现库跟着一起装：否则 install 之后链 mylib_image 会缺 stbi_* 符号
    list(APPEND MYLIB_TARGETS mylib_image mylib_stb_impl)
endif()

# ---------------------------------------------------------------- cv（OpenCV）
if(MYLIB_ENABLE_CV AND MYLIB_HAVE_CV)
    # 以前这里无条件链 mylib_infer，导致 -DMYLIB_ENABLE_INFER=OFF 时
    # cv 跟着编不出来（P5"组件独立开关"在那一格是假的）。cv 真正需要的是
    # preprocess 里的 letterbox/张量转换，infer 只是 Shape 的实现，按需挂。
    set(_cv_link mylib_core mylib_preprocess deps::opencv)
    if(TARGET mylib_infer)
        list(APPEND _cv_link mylib_infer)
    endif()
    mylib_add_component(cv
        SOURCES cv/image.cpp
        LINK ${_cv_link})
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
