# =============================================================================
#  可配置选项
# =============================================================================
# =============================================================================
#  是否作为顶层工程构建（决定示例/测试的默认开关）
# =============================================================================
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(MYLIB_IS_TOP_LEVEL TRUE)
else()
    set(MYLIB_IS_TOP_LEVEL FALSE)
endif()

option(MYLIB_BUILD_EXAMPLES "构建示例程序" ${MYLIB_IS_TOP_LEVEL})
option(MYLIB_BUILD_TESTS    "构建单元测试" ${MYLIB_IS_TOP_LEVEL})
option(MYLIB_BUILD_TOOLS    "构建命令行工具（model_inspect 等）" ${MYLIB_IS_TOP_LEVEL})
option(MYLIB_INSTALL       "生成安装规则（作为子目录嵌入时默认关闭，避免污染宿主工程的安装前缀）" ${MYLIB_IS_TOP_LEVEL})

# 组件开关：按目标平台裁剪，例如 Zynq 上关掉 infer
option(MYLIB_ENABLE_CORE  "核心组件（Status/Result/平台信息）" ON)
option(MYLIB_ENABLE_LOG   "日志组件（封装 spdlog）"            ON)
option(MYLIB_ENABLE_JSON  "JSON 组件（封装 nlohmann/json）"     ON)
option(MYLIB_ENABLE_MATH  "数学组件（封装 Eigen）"             ON)
option(MYLIB_ENABLE_INFER "推理组件（封装 ONNX Runtime / ncnn）" ON)
option(MYLIB_ENABLE_ONNXRUNTIME "接入 ONNX Runtime 后端"          ON)
option(MYLIB_ENABLE_NCNN  "接入 ncnn 作为备选推理后端"          OFF)
option(MYLIB_ENABLE_DSP   "信号处理组件（频谱/滤波/检测/识别/解调，pocketfft）" ON)
option(MYLIB_ENABLE_ML    "机器学习组件（sklearn→ONNX 表格模型推理，依赖 infer+json）" ON)
option(MYLIB_ENABLE_NET   "网络组件（TCP/UDP，无三方依赖）" ON)
option(MYLIB_ENABLE_HTTP  "HTTP 组件（客户端+服务端，封装 cpp-httplib）" ON)
option(MYLIB_ENABLE_IMAGE "图像组件（stb_image 解码/缩放/预处理，不依赖 OpenCV）" ON)
option(MYLIB_ENABLE_CV    "图像组件（封装 OpenCV）"             OFF)

# ncnn / OpenCV 的手工路径（留空则自动在 third_party 里找）
set(MYLIB_NCNN_ROOT "" CACHE PATH "ncnn 安装根目录（含 include/ncnn 与 lib）")
set(MYLIB_OPENCV_ROOT "" CACHE PATH "OpenCV 安装根目录（含 OpenCVConfig.cmake）")

# 构建共享库而不是静态库（嵌入式默认静态）
option(MYLIB_BUILD_SHARED "构建动态库" OFF)

# 消毒器：空 / address（内存错误）/ undefined（未定义行为，trap 模式）
set(MYLIB_SANITIZE "" CACHE STRING "消毒器：留空 / address / undefined")
set_property(CACHE MYLIB_SANITIZE PROPERTY STRINGS "" address undefined)

# 以 -fno-exceptions 编译本库（**只支持 core + math + dsp + net**，见文件末尾的校验）
option(MYLIB_NO_EXCEPTIONS "以 -fno-exceptions 编译本库（只支持 core + math + dsp + net）" OFF)

# 三方依赖目录（默认在源码树 third_party，由 resources/*.zip 自动解压）
set(MYLIB_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party" CACHE PATH "已解压的三方库根目录")
set(MYLIB_RESOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources" CACHE PATH "三方库压缩包目录")
option(MYLIB_AUTO_EXTRACT_DEPS "configure 时自动从 resources 解压缺失的三方库" ON)

# ONNX Runtime 预编译包位置（留空则自动搜索 third_party/onnxruntime-*）
set(MYLIB_ONNXRUNTIME_ROOT "" CACHE PATH "ONNX Runtime 预编译包根目录（含 include/ 与 lib/）")

# -----------------------------------------------------------------------------
# 统一确定库类型与可见性
# -----------------------------------------------------------------------------
if(MYLIB_BUILD_SHARED)
    message(FATAL_ERROR
        "MYLIB_BUILD_SHARED=ON 暂不支持：公共头里还有50+ 个对外符号没标 MYLIB_API，\n"
        "开启 hidden visibility 之后消费方会链接失败。推荐用默认的静态库（add_subdirectory\n"
        "源码分发，换平台重编，本来就没有 ABI 问题）。原因与待办见 docs/03 §9。")
    set(_mylib_lib_type SHARED)
    set(MYLIB_SHARED_DEFINE ON)
else()
    set(_mylib_lib_type STATIC)
    set(MYLIB_SHARED_DEFINE OFF)
endif()

# -----------------------------------------------------------------------------
# -fno-exceptions 构建
#
# 为什么只承诺 core + math + dsp + net：其余组件的错误上报机制**本身就是异常**
#   * json ：nlohmann 不带 JSON_NOEXCEPTION 时 `throw` 直接编不过；
#            带上它又变成 std::abort()，于是"解析失败返回 ParseError"变成"崩溃"，
#            Status/Result 的契约当场失效。
#   * infer：ONNX Runtime 的 C++ API 用 Ort::Exception 上报，调用处必须 catch；
#            ncnn / stub 虽然不抛，但只要开着 ORT 后端就编不过。
#   * cv   ：OpenCV 用 cv::Exception 上报，官方包也是按开异常编的。
#   * image：本身不抛，但 stb 的失败路径是返回 null + 字符串，理论上可以支持；
#            目前没验证过，先不放进承诺里。
#   * log  ：SPDLOG_NO_EXCEPTIONS 能把编译打开，但"日志文件打不开"会退化成 abort，
#            同样和"错误必须被返回"的设计原则冲突。
# dsp 用的是 pocketfft，已经在 Deps 里定义 POCKETFFT_NO_EXCEPTIONS（走 abort），
# 自己一行 try/catch 都没有，所以可以进这个子集。
#
# 所以这里的做法是：**把承诺收紧到能验证的范围**，并在 configure 阶段
# 就把不成立的组合拦下来，给出一句人能看懂的原因，而不是让用户对着
# 一屏模板报错猜。CI 里有一个 job 专门跑这个子集（见 .github/workflows/ci.yml）。
# -----------------------------------------------------------------------------
add_library(mylib_noexcept INTERFACE)
add_library(my_cpp_libs::noexcept ALIAS mylib_noexcept)

if(MYLIB_NO_EXCEPTIONS)
    set(_noex_blocked "")
    foreach(_comp LOG JSON INFER CV IMAGE HTTP)
        if(MYLIB_ENABLE_${_comp})
            list(APPEND _noex_blocked "${_comp}")
        endif()
    endforeach()

    if(_noex_blocked)
        string(REPLACE ";" " / " _noex_str "${_noex_blocked}")
        message(FATAL_ERROR
            "MYLIB_NO_EXCEPTIONS=ON 目前只支持 core + math + dsp + net，但以下组件还开着：${_noex_str}\n"
            "\n"
            "原因：这些组件用异常上报错误（nlohmann / ONNX Runtime / OpenCV / spdlog），\n"
            "      关掉异常后只能退化成 abort()，会破坏 Status/Result 的契约。\n"
            "      取舍与真值表见 docs/07-图像链路与无异常构建.md\n"
            "\n"
            "可用的最小配置：\n"
            "  -DMYLIB_NO_EXCEPTIONS=ON \\\n"
            "  -DMYLIB_ENABLE_LOG=OFF -DMYLIB_ENABLE_JSON=OFF \\\n"
            "  -DMYLIB_ENABLE_INFER=OFF -DMYLIB_ENABLE_IMAGE=OFF -DMYLIB_ENABLE_CV=OFF \\\n"
            "  -DMYLIB_ENABLE_HTTP=OFF")
    endif()

    target_compile_definitions(mylib_noexcept INTERFACE MYLIB_NO_EXCEPTIONS=1)
    if(MSVC)
        # /EHs-c- = 关掉 C++ 异常处理语法；_HAS_EXCEPTIONS=0 让 STL 走无异常分支
        target_compile_options(mylib_noexcept INTERFACE /EHs-c-)
        target_compile_definitions(mylib_noexcept INTERFACE _HAS_EXCEPTIONS=0)
    else()
        target_compile_options(mylib_noexcept INTERFACE -fno-exceptions)
    endif()
endif()
