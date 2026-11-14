#pragma once
// =============================================================================
//  汇总头：一次引入所有组件
//
//  需要链接 my_cpp_libs::all（会带上 Eigen / nlohmann / spdlog 的头文件路径）。
//  只用某个组件时，请直接包含对应子头，不要用这个汇总头。
// =============================================================================

#include "my_cpp_libs/core/build_info.hpp"
#include "my_cpp_libs/core/status.hpp"

#include "my_cpp_libs/log/log.hpp"

#include "my_cpp_libs/json/json.hpp"

#include "my_cpp_libs/math/math.hpp"

#include "my_cpp_libs/infer/infer.hpp"
#include "my_cpp_libs/infer/preprocess.hpp"

// cv 组件需要链接 my_cpp_libs::cv，且依赖 OpenCV
#if defined(MYLIB_ENABLE_CV_COMPONENT)
#  include "my_cpp_libs/cv/image.hpp"
#endif

#define MY_CPP_LIBS_VERSION "0.1.0"
