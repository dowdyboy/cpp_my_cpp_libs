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

# 组件开关：按目标平台裁剪，例如 Zynq 上关掉 infer
option(MYLIB_ENABLE_CORE  "核心组件（Status/Result/平台信息）" ON)
option(MYLIB_ENABLE_LOG   "日志组件（封装 spdlog）"            ON)
option(MYLIB_ENABLE_JSON  "JSON 组件（封装 nlohmann/json）"     ON)
option(MYLIB_ENABLE_MATH  "数学组件（封装 Eigen）"             ON)
option(MYLIB_ENABLE_INFER "推理组件（封装 ONNX Runtime / ncnn）" ON)
option(MYLIB_ENABLE_ONNXRUNTIME "接入 ONNX Runtime 后端"          ON)
option(MYLIB_ENABLE_NCNN  "接入 ncnn 作为备选推理后端"          OFF)
option(MYLIB_ENABLE_CV    "图像组件（封装 OpenCV）"             OFF)

# ncnn / OpenCV 的手工路径（留空则自动在 third_party 里找）
set(MYLIB_NCNN_ROOT "" CACHE PATH "ncnn 安装根目录（含 include/ncnn 与 lib）")
set(MYLIB_OPENCV_ROOT "" CACHE PATH "OpenCV 安装根目录（含 OpenCVConfig.cmake）")

# 构建共享库而不是静态库（嵌入式默认静态）
option(MYLIB_BUILD_SHARED "构建动态库" OFF)

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
    set(_mylib_lib_type SHARED)
    set(MYLIB_SHARED_DEFINE ON)
else()
    set(_mylib_lib_type STATIC)
    set(MYLIB_SHARED_DEFINE OFF)
endif()
