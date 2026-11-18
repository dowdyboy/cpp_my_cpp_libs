#include "my_cpp_libs/dsp/dsp.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "common.hpp"

namespace my::dsp {
namespace {

using detail::kPi;
using detail::kTwoPi;

/// 低通（二阶），cutoff <= 0 时用 fs/200 作为"音频/基带"默认
void lowpass_inplace(Samples& x, double cutoff_hz, double sample_rate) {
    const double fc = (cutoff_hz > 0.0) ? cutoff_hz : sample_rate / 200.0;
    if (fc <= 0.0 || fc >= sample_rate / 2.0) return;
    auto bq = biquad_design(BiquadType::Lowpass, fc, sample_rate, 0.70710678);
    if (!bq) return;
    biquad_apply(bq.value(), x);
}

void remove_mean(Samples& x) {
    if (x.empty()) return;
    const double mean = std::accumulate(x.begin(), x.end(), 0.0) / static_cast<double>(x.size());
    for (auto& v : x) v = static_cast<float>(static_cast<double>(v) - mean);
}

/// 单音/基带信号的包络
Result<Samples> envelope_of(const Samples& x) {
    auto a = detail::analytic(x);
    if (!a) return a.status();
    Samples env(a.value().size());
    for (std::size_t i = 0; i < env.size(); ++i) env[i] = std::abs(a.value()[i]);
    return env;
}

/// 瞬时频率（Hz）。absolute=true 时保留载波中心频率 —— 解调 FSK 这种
/// 用绝对 mark/space 的场合必须这样，否则频率都被搬到了 0 附近。
Result<Samples> inst_freq_hz(const Samples& x, double sample_rate, bool absolute = false) {
    auto a = detail::analytic(x);
    if (!a) return a.status();
    auto df = detail::instantaneous_frequency(a.value(), /*remove_mean=*/!absolute);
    if (!df) return df.status();
    const double scale = sample_rate / kTwoPi;
    Samples      out(df.value().size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<float>(df.value()[i] * scale);
    }
    return out;
}

/// 按给定定时偏移做复信号的积分清零
std::vector<Complex> integrate_dump_complex(const CSamples& x, double sps, double offset) {
    std::vector<Complex> out;
    if (sps <= 1.0 || offset < 0.0) return out;
    // offset 允许大于信号长度（sps 很大时定时搜索范围会超出信号），必须先挡住：
    // 否则 (x.size() - offset) 为负，static_cast<size_t> 是未定义行为，
    // 紧接着 reserve(巨大值) 会抛 length_error 穿透 Result 边界。
    if (offset >= static_cast<double>(x.size())) return out;
    const std::size_t nsym = static_cast<std::size_t>(
        (static_cast<double>(x.size()) - offset) / sps);
    out.reserve(nsym);
    for (std::size_t k = 0; k < nsym; ++k) {
        const auto begin = static_cast<std::size_t>(
            std::llround(offset + static_cast<double>(k) * sps));
        const auto end = std::min<std::size_t>(
            x.size(),
            static_cast<std::size_t>(std::llround(offset + static_cast<double>(k + 1) * sps)));
        if (end <= begin) continue;
        Complex sum(0.0f, 0.0f);
        for (std::size_t i = begin; i < end; ++i) sum += x[i];
        out.push_back(sum / static_cast<float>(end - begin));
    }
    return out;
}

/// 符号间方差：定时对齐时最大（ASK 在 0/A 之间跳，FSK 在两个频率之间跳）
double symbol_variance(const Samples& sym) {
    if (sym.size() < 4) return -1.0;
    double mean = 0.0;
    for (float v : sym) mean += v;
    mean /= static_cast<double>(sym.size());
    double var = 0.0;
    for (float v : sym) var += (static_cast<double>(v) - mean) * (static_cast<double>(v) - mean);
    return var / static_cast<double>(sym.size());
}

/// 在 [0, sps) 内搜最佳符号定时。
///
/// 为什么需要它：突发检测给出的起点是按窗口算的，可能比真正的符号边界早
/// 半个符号；ASK/FSK 的解调是积分清零，定时偏了会同时吃到相邻符号。
Result<Samples> integrate_dump_best(const Samples& x, double sps) {
    Samples best;
    double  best_var = -1.0;
    for (int q = 0; q < 4; ++q) {
        auto sym = detail::integrate_dump(x, sps, static_cast<double>(q) * sps / 4.0);
        if (!sym) continue;
        const double var = symbol_variance(sym.value());
        if (var > best_var) {
            best_var = var;
            best     = sym.value();
        }
    }
    if (best.empty()) return Status(Errc::RuntimeError, "符号数不足（检查符号率）");
    return best;
}

/// 相位相干度：BPSK 的符号平方后应当同相，用 |Σs²|/Σ|s|² 衡量
double coherence(const std::vector<Complex>& syms) {
    if (syms.empty()) return 0.0;
    Complex sq_sum(0.0f, 0.0f);
    double  energy = 0.0;
    for (const auto& s : syms) {
        sq_sum += s * s;
        energy += std::norm(s);
    }
    if (energy <= 0.0) return 0.0;
    return std::abs(sq_sum) / energy;
}

/// 平均符号功率：用来挑定时。**不能只靠 coherence** —— BPSK 的符号只要落
/// 在实轴上（哪怕一半取自相邻符号），coherence 都是 1，定时错一个符号也看不出来。
double mean_power(const std::vector<Complex>& syms) {
    if (syms.empty()) return 0.0;
    double e = 0.0;
    for (const auto& s : syms) e += std::norm(s);
    return e / static_cast<double>(syms.size());
}

}  // namespace

// -----------------------------------------------------------------------------
//  模拟调制
// -----------------------------------------------------------------------------
Result<Samples> demod_am(const Samples& x, double sample_rate, double audio_cutoff_hz) {
    if (x.size() < 32) return Status(Errc::InvalidArgument, "样本太少");
    if (sample_rate <= 0.0) return Status(Errc::InvalidArgument, "采样率必须为正");

    auto env = envelope_of(x);
    if (!env) return env.status();
    Samples out = env.value();
    remove_mean(out);
    lowpass_inplace(out, audio_cutoff_hz, sample_rate);
    return out;
}

Result<Samples> demod_fm(const Samples& x, double sample_rate, double audio_cutoff_hz) {
    if (x.size() < 32) return Status(Errc::InvalidArgument, "样本太少");
    if (sample_rate <= 0.0) return Status(Errc::InvalidArgument, "采样率必须为正");

    auto f = inst_freq_hz(x, sample_rate);
    if (!f) return f.status();
    Samples out = f.value();
    remove_mean(out);
    lowpass_inplace(out, audio_cutoff_hz, sample_rate);
    return out;
}

// -----------------------------------------------------------------------------
//  数字调制
// -----------------------------------------------------------------------------
Result<std::vector<std::uint8_t>> demod_ask(const Samples& x, double sample_rate,
                                            double symbol_rate, float threshold) {
    if (x.size() < 32) return Status(Errc::InvalidArgument, "样本太少");
    if (sample_rate <= 0.0 || symbol_rate <= 0.0) {
        return Status(Errc::InvalidArgument, "采样率/符号率必须为正");
    }
    const double sps = sample_rate / symbol_rate;
    if (sps < 2.0) return Status(Errc::InvalidArgument, "每符号采样数不足，至少 2");

    auto env = envelope_of(x);
    if (!env) return env.status();
    auto sym = integrate_dump_best(env.value(), sps);
    if (!sym) return sym.status();

    const auto [lo_it, hi_it] = std::minmax_element(sym.value().begin(), sym.value().end());
    const float lo = *lo_it;
    const float hi = *hi_it;
    if (threshold < 0.0f) threshold = 0.5f * (lo + hi);

    std::vector<std::uint8_t> bits(sym.value().size());
    for (std::size_t k = 0; k < bits.size(); ++k) {
        bits[k] = (sym.value()[k] > threshold) ? 1u : 0u;
    }
    return bits;
}

Result<std::vector<std::uint8_t>> demod_fsk(const Samples& x, double sample_rate,
                                            double symbol_rate, double mark_hz, double space_hz) {
    if (x.size() < 32) return Status(Errc::InvalidArgument, "样本太少");
    if (sample_rate <= 0.0 || symbol_rate <= 0.0) {
        return Status(Errc::InvalidArgument, "采样率/符号率必须为正");
    }
    const double sps = sample_rate / symbol_rate;
    if (sps < 2.0) return Status(Errc::InvalidArgument, "每符号采样数不足，至少 2");

    auto f = inst_freq_hz(x, sample_rate, /*absolute=*/true);
    if (!f) return f.status();
    auto sym = integrate_dump_best(f.value(), sps);
    if (!sym) return sym.status();

    std::vector<std::uint8_t> bits(sym.value().size());
    for (std::size_t k = 0; k < bits.size(); ++k) {
        const double d_mark  = std::fabs(static_cast<double>(sym.value()[k]) - mark_hz);
        const double d_space = std::fabs(static_cast<double>(sym.value()[k]) - space_hz);
        bits[k]              = (d_mark < d_space) ? 1u : 0u;
    }
    return bits;
}

Result<std::vector<std::uint8_t>> demod_bpsk(const CSamples& x, double sample_rate,
                                             double symbol_rate) {
    if (x.size() < 64) return Status(Errc::InvalidArgument, "样本太少");
    if (sample_rate <= 0.0 || symbol_rate <= 0.0) {
        return Status(Errc::InvalidArgument, "采样率/符号率必须为正");
    }
    const double sps = sample_rate / symbol_rate;
    if (sps < 4.0) return Status(Errc::InvalidArgument, "每符号采样数不足，BPSK 至少 4");

    // ---- 1) 平方环粗估载波频偏：x² 是一个纯音，频率 = 2×频偏 ----
    const std::size_t n = x.size();
    CSamples          sq(n);
    for (std::size_t i = 0; i < n; ++i) sq[i] = x[i] * x[i];

    auto spec = fft(sq, detail::next_pow2(n * 4));  // 补零 4 倍：频偏分辨率更细
    if (!spec) return spec.status();
    std::size_t peak_bin = 0;
    double      peak_mag = 0.0;
    for (std::size_t k = 0; k < spec.value().size(); ++k) {
        const double mag = std::abs(spec.value()[k]);
        if (mag > peak_mag) {
            peak_mag = mag;
            peak_bin = k;
        }
    }
    const double nfft   = static_cast<double>(spec.value().size());
    const double bin_hz = sample_rate / nfft;
    const double f2     = (peak_bin <= spec.value().size() / 2)
                              ? static_cast<double>(peak_bin) * bin_hz
                              : (static_cast<double>(peak_bin) - nfft) * bin_hz;
    const double cfo_mag = std::fabs(f2) / 2.0;

    // ---- 2) 频偏符号 + 符号定时：两个候选各试一遍，用相干度×功率挑 ----
    //
    // 平方环只能得到 |cfo|，符号（正负）丢了；符号挑错会让符号相位逐符号旋转，
    // 相干度 |Σs²|/Σ|s|² 会明显下降。定时则靠平均功率（对齐时每个符号窗
    // 都刚好覆盖一个符号，功率最大）。
    std::vector<Complex> best_syms;
    double               best_metric = -1.0;

    for (int sign = -1; sign <= 1; sign += 2) {
        const double cfo  = (sign < 0) ? -cfo_mag : cfo_mag;
        const double dphi = -kTwoPi * cfo / sample_rate;
        CSamples     derot(n);
        double       phase = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            derot[i] = x[i] * Complex(static_cast<float>(std::cos(phase)),
                                      static_cast<float>(std::sin(phase)));
            phase += dphi;
        }

        for (int q = 0; q < 4; ++q) {
            const double offset = static_cast<double>(q) * sps / 4.0;
            auto         syms   = integrate_dump_complex(derot, sps, offset);
            if (syms.size() < 8) continue;
            const double metric = coherence(syms) * mean_power(syms);
            if (metric > best_metric) {
                best_metric = metric;
                best_syms   = std::move(syms);
            }
        }
    }

    if (best_syms.size() < 8) return Status(Errc::RuntimeError, "符号数不足（检查符号率）");

    // ---- 3) 残余公共相位：BPSK 的 s_k = ±A e^{jθ}，s_k² 与比特无关 ----
    Complex sq_sum(0.0f, 0.0f);
    for (const auto& s : best_syms) sq_sum += s * s;
    const double  theta = 0.5 * std::arg(sq_sum);
    const Complex rot(std::cos(static_cast<float>(-theta)), std::sin(static_cast<float>(-theta)));

    std::vector<std::uint8_t> bits(best_syms.size());
    for (std::size_t k = 0; k < best_syms.size(); ++k) {
        const Complex s = best_syms[k] * rot;
        bits[k]         = (s.real() > 0.0f) ? 1u : 0u;
    }
    return bits;
}

std::vector<std::uint8_t> slice_bits(const Samples& soft) {
    std::vector<std::uint8_t> bits(soft.size());
    for (std::size_t i = 0; i < soft.size(); ++i) {
        bits[i] = (soft[i] > 0.5f) ? 1u : 0u;
    }
    return bits;
}

}  // namespace my::dsp
