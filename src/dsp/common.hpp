#pragma once
// =============================================================================
//  my::dsp 内部工具（不进公共头）
// =============================================================================
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "my_cpp_libs/dsp/dsp.hpp"

namespace my::dsp::detail {

constexpr double kPi      = 3.14159265358979323846;
constexpr double kTwoPi   = 6.28318530717958647692;
constexpr float  kEpsF    = 1e-12f;

inline float power_to_db(float p) { return 10.0f * std::log10(std::max(p, 1e-20f)); }
inline float amp_to_db(float a) { return 20.0f * std::log10(std::max(a, 1e-10f)); }

/// 解析信号（Hilbert）：FFT -> 只保留正频率 -> IFFT。
/// 实信号的包络/瞬时频率都从它算，比正交下变频省事且没有镜像。
Result<CSamples> analytic(const Samples& x);

/// 相位解缠（就地）
void unwrap_phase(Samples& phase);

/// 中位数（会拷贝入参）
float median(std::vector<float> v);

/// 双峰度：值落进两个主峰（按直方图前两名）附近的比例，[0,1]
double two_level_ratio(const float* v, std::size_t n, int bins = 32);

/// 不小于 n 的 2 的幂
std::size_t next_pow2(std::size_t n);

/// 瞬时频率（rad/sample）：解析信号的相邻相位差。
/// remove_mean = true 时去掉直流（特征分析用）；解调要**绝对频率**时传 false，
/// 否则 mark/space 这类绝对频率参数没法比较。
Result<Samples> instantaneous_frequency(const CSamples& analytic_sig, bool remove_mean = true);

/// 按符号率做积分清零（每符号取一个平均值），sps 可以是小数。
/// offset 是窗起点在符号内的平移（0 ~ sps），解调时用来搜索最佳定时。
Result<Samples> integrate_dump(const Samples& x, double samples_per_symbol, double offset = 0.0);

}  // namespace my::dsp::detail
