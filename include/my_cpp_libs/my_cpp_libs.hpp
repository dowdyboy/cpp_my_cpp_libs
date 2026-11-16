#pragma once
// =============================================================================
//  汇总头：一次引入**当前构建里开启的**组件
//
//  每个组件都按 MYLIB_ENABLE_<组件>_COMPONENT 宏守卫，所以裁剪过的构建
//  （比如只开 core+log）包含它不会因为缺 Eigen/nlohmann 头而编译失败。
//  只用一个组件时，直接包含对应子头更省编译时间。
// =============================================================================

#include "my_cpp_libs/core/build_info.hpp"
#include "my_cpp_libs/core/status.hpp"

#if defined(MYLIB_ENABLE_LOG_COMPONENT)
#  include "my_cpp_libs/log/log.hpp"
#endif

#if defined(MYLIB_ENABLE_JSON_COMPONENT)
#  include "my_cpp_libs/json/json.hpp"
#endif

#if defined(MYLIB_ENABLE_MATH_COMPONENT)
#  include "my_cpp_libs/math/math.hpp"
#endif

#if defined(MYLIB_ENABLE_INFER_COMPONENT)
#  include "my_cpp_libs/infer/infer.hpp"
#  include "my_cpp_libs/infer/preprocess.hpp"
#endif

// image 组件（stb 解码 + 预处理），板端没有 OpenCV 时也能用
#if defined(MYLIB_ENABLE_IMAGE_COMPONENT)
#  include "my_cpp_libs/image/image.hpp"
#endif

// dsp 组件（频谱/滤波/检测/识别/解调）
#if defined(MYLIB_ENABLE_DSP_COMPONENT)
#  include "my_cpp_libs/dsp/dsp.hpp"
#endif

// net 组件（TCP/UDP）
#if defined(MYLIB_ENABLE_NET_COMPONENT)
#  include "my_cpp_libs/net/net.hpp"
#endif

// http 组件（客户端+服务端，封装 cpp-httplib）
#if defined(MYLIB_ENABLE_HTTP_COMPONENT)
#  include "my_cpp_libs/http/http.hpp"
#endif

// ml 组件（sklearn→ONNX 表格模型推理）
#if defined(MYLIB_ENABLE_ML_COMPONENT)
#  include "my_cpp_libs/ml/ml.hpp"
#endif

// cv 组件需要链接 my_cpp_libs::cv，且依赖 OpenCV
#if defined(MYLIB_ENABLE_CV_COMPONENT)
#  include "my_cpp_libs/cv/image.hpp"
#endif

