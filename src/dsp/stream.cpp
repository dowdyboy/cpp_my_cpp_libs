#include "my_cpp_libs/dsp/stream.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

#include "common.hpp"

namespace my::dsp {

namespace {

using detail::kTwoPi;

/// 有限正数校验：不合法时回退到默认值（AGC 构造函数没有 Status 返回值）。
double positive_finite_or(double value, double fallback) {
    return (std::isfinite(value) && value > 0.0) ? value : fallback;
}

/// 一阶平滑系数：时间常数 tau 秒、采样率 fs Hz -> 每样本的 alpha。
double one_pole_alpha(double tau_sec, double sample_rate) {
    return 1.0 - std::exp(-1.0 / (tau_sec * sample_rate));
}

}  // namespace

// =============================================================================
//  FIR
// =============================================================================
FirFilter::FirFilter(Fir fir)
    : fir_(std::move(fir)),
      history_(fir_.taps.size() > 1 ? fir_.taps.size() - 1 : 0, 0.0f) {}

Result<FirFilter> FirFilter::create(const Fir& fir) {
    if (fir.taps.empty()) return Status(Errc::InvalidArgument, "FIR 抽头为空");
    for (float tap : fir.taps) {
        if (!std::isfinite(tap)) return Status(Errc::InvalidArgument, "FIR 抽头包含非有限值");
    }
    return FirFilter(fir);
}

Result<Samples> FirFilter::process(const Samples& in) {
    if (fir_.taps.empty()) return Status(Errc::InvalidArgument, "FIR 抽头为空");
    if (in.empty()) return Samples{};

    const std::size_t m      = fir_.taps.size();
    const std::size_t d      = fir_.delay();
    const std::size_t n_prev = total_in_;
    const std::size_t n_new  = n_prev + in.size();

    // combined = 历史（最近 m-1 个输入，初始为零）+ 本块。
    // 全局下标 j 对应 combined[j - n_prev + (m-1)]。
    std::vector<float> combined;
    combined.reserve(m - 1 + in.size());
    combined.insert(combined.end(), history_.begin(), history_.end());
    combined.insert(combined.end(), in.begin(), in.end());

    // 批量 fir_apply 的 y[i] = z[i+delay]，其中 z 是因果卷积（两端零延拓）。
    // 本次块能给出的最新下标是 n_new-1，最老下标是 max(delay, n_prev)：
    // 延迟线未填满前不输出（下标 < delay 对应批量版的负下标）。
    Samples           out;
    const std::size_t start = (n_prev < d) ? d : n_prev;
    if (n_new > start) {
        out.resize(n_new - start);
        for (std::size_t k = start; k < n_new; ++k) {
            double acc = 0.0;
            for (std::size_t j = 0; j < m; ++j) {
                const std::size_t idx = k - j + (m - 1) - n_prev;
                acc += static_cast<double>(fir_.taps[j]) * static_cast<double>(combined[idx]);
            }
            out[k - start] = static_cast<float>(acc);
        }
    }

    if (m > 1) {
        std::copy(combined.end() - static_cast<std::ptrdiff_t>(m - 1), combined.end(),
                  history_.begin());
    }
    total_in_ = n_new;
    return out;
}

Result<Samples> FirFilter::flush() {
    if (fir_.taps.empty()) return Status(Errc::InvalidArgument, "FIR 抽头为空");
    const std::size_t d = delay();
    if (d == 0) return Samples{};

    // 批量版在整段末尾按零延拓；这里显式喂 d 个零，正好补齐最后 d 个输出。
    Samples zeros(d, 0.0f);
    auto    tail = process(zeros);
    reset();
    return tail;
}

void FirFilter::reset() noexcept {
    std::fill(history_.begin(), history_.end(), 0.0f);
    total_in_ = 0;
}

std::size_t FirFilter::taps() const noexcept { return fir_.taps.size(); }

std::size_t FirFilter::delay() const noexcept { return fir_.delay(); }

// =============================================================================
//  级联双二阶
// =============================================================================
BiquadFilter::BiquadFilter(std::vector<Biquad> stages)
    : stages_(std::move(stages)), z1_(stages_.size(), 0.0f), z2_(stages_.size(), 0.0f) {}

Result<BiquadFilter> BiquadFilter::create(std::vector<Biquad> stages) {
    if (stages.empty()) return Status(Errc::InvalidArgument, "级联至少需要一级 biquad");
    for (const auto& bq : stages) {
        if (!std::isfinite(bq.b0) || !std::isfinite(bq.b1) || !std::isfinite(bq.b2) ||
            !std::isfinite(bq.a1) || !std::isfinite(bq.a2)) {
            return Status(Errc::InvalidArgument, "biquad 系数包含非有限值");
        }
    }
    return BiquadFilter(std::move(stages));
}

Result<Samples> BiquadFilter::process(const Samples& in) {
    Samples out = in;
    process_inplace(out);
    return out;
}

void BiquadFilter::process_inplace(Samples& x) {
    if (x.empty()) return;

    // 与批量 biquad_apply() 完全相同的转置直接 II 型；逐级串联，
    // 状态跨块保留，所以分块与整段的结果一致。
    for (std::size_t s = 0; s < stages_.size(); ++s) {
        const Biquad& bq = stages_[s];
        float         z1 = z1_[s];
        float         z2 = z2_[s];
        for (auto& sample : x) {
            const float in  = sample;
            const float out = bq.b0 * in + z1;
            z1              = bq.b1 * in - bq.a1 * out + z2;
            z2              = bq.b2 * in - bq.a2 * out;
            sample          = out;
        }
        z1_[s] = z1;
        z2_[s] = z2;
    }
}

void BiquadFilter::reset() noexcept {
    std::fill(z1_.begin(), z1_.end(), 0.0f);
    std::fill(z2_.begin(), z2_.end(), 0.0f);
}

std::size_t BiquadFilter::stages() const noexcept { return stages_.size(); }

// =============================================================================
//  复本振 / 混频
// =============================================================================
ComplexMixer::ComplexMixer(double dphi, double initial_phase) noexcept
    : dphi_(dphi), phase_(initial_phase), initial_phase_(initial_phase) {}

Result<ComplexMixer> ComplexMixer::create(double freq_hz, double sample_rate,
                                          double initial_phase) {
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0) {
        return Status(Errc::InvalidArgument, "采样率必须为正");
    }
    if (!std::isfinite(freq_hz) || freq_hz < 0.0) {
        return Status(Errc::InvalidArgument, "混频频率必须为非负有限值");
    }
    if (freq_hz >= sample_rate / 2.0) {
        return Status(Errc::InvalidArgument, "混频频率必须在 [0, fs/2) 内");
    }
    if (!std::isfinite(initial_phase)) {
        return Status(Errc::InvalidArgument, "初始相位必须是有限值");
    }
    return ComplexMixer(-kTwoPi * freq_hz / sample_rate, initial_phase);
}

Result<CSamples> ComplexMixer::process(const Samples& in) {
    if (in.empty()) return CSamples{};

    CSamples          out(in.size());
    double            phase = phase_;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const double v = static_cast<double>(in[i]);
        out[i]         = Complex(static_cast<float>(v * std::cos(phase)),
                                 static_cast<float>(v * std::sin(phase)));
        phase += dphi_;
    }
    phase_ = phase;
    return out;
}

Result<CSamples> ComplexMixer::process(const CSamples& in) {
    if (in.empty()) return CSamples{};

    CSamples          out(in.size());
    double            phase = phase_;
    for (std::size_t i = 0; i < in.size(); ++i) {
        out[i] = in[i] * Complex(static_cast<float>(std::cos(phase)),
                                 static_cast<float>(std::sin(phase)));
        phase += dphi_;
    }
    phase_ = phase;
    return out;
}

void ComplexMixer::reset() noexcept { phase_ = initial_phase_; }

double ComplexMixer::phase() const noexcept { return phase_; }

// =============================================================================
//  抗混叠 FIR + 抽取
// =============================================================================
Decimator::Decimator(int factor, Fir anti_alias)
    : factor_(factor), fir_(std::move(anti_alias)) {}

Result<Decimator> Decimator::create(int factor, double sample_rate) {
    if (factor < 1) return Status(Errc::InvalidArgument, "factor 必须 >= 1");
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0) {
        return Status(Errc::InvalidArgument, "采样率必须为正");
    }

    // 与批量 decimate() 的 factor == 1 分支一致：直通，不做任何滤波。
    if (factor == 1) return Decimator(1, Fir{{1.0f}});

    // 批量 decimate() 的滤波器设计逐参数照搬：截止 0.9*新奈奎斯特，
    // 抽头数 16*factor+1，Hamming 窗（见 src/dsp/filter.cpp）。
    const double new_nyquist = sample_rate / (2.0 * static_cast<double>(factor));
    const std::size_t num_taps =
        static_cast<std::size_t>(16) * static_cast<std::size_t>(factor) + 1;
    auto lp = fir_lowpass(0.9 * new_nyquist, sample_rate, num_taps, Window::Hamming);
    if (!lp) return lp.status();
    return Decimator(factor, lp.value());
}

void Decimator::collect(std::size_t begin, std::size_t end, std::size_t limit,
                        const Samples& filtered, Samples& out) const {
    if (begin >= end) return;
    const std::size_t f = static_cast<std::size_t>(factor_);
    std::size_t       k = begin;
    const std::size_t rem = k % f;
    if (rem != 0) k += f - rem;
    for (; k < end && k < limit; k += f) {
        out.push_back(filtered[k - begin]);
    }
}

Result<Samples> Decimator::process(const Samples& in) {
    if (in.empty()) return Samples{};

    auto filtered = fir_.process(in);
    if (!filtered) return filtered.status();

    const std::size_t begin = filtered_count_;
    filtered_count_ += filtered.value().size();
    input_count_ += in.size();

    Samples out;
    collect(begin, filtered_count_, input_count_, filtered.value(), out);
    return out;
}

Result<Samples> Decimator::flush() {
    Samples out;
    if (input_count_ == 0) {
        reset();
        return out;
    }

    // 内部 FIR 的群延迟 = 8*factor 个输入样本；喂同样多的零即可补出批量版
    // 末尾的那几个完整符号。collect 的 limit 用真实输入数，避免把"补零区"
    // 当成新的符号输出（批量版只对 [0, N) 抽取）。
    const std::size_t d = fir_.delay();
    if (d > 0) {
        Samples zeros(d, 0.0f);
        auto    filtered = fir_.process(zeros);
        if (!filtered) return filtered.status();

        const std::size_t begin = filtered_count_;
        filtered_count_ += filtered.value().size();
        collect(begin, filtered_count_, input_count_, filtered.value(), out);
    }
    reset();
    return out;
}

void Decimator::reset() noexcept {
    fir_.reset();
    input_count_    = 0;
    filtered_count_ = 0;
}

int Decimator::factor() const noexcept { return factor_; }

// =============================================================================
//  AGC
// =============================================================================
Agc::Agc(Options o) : opt_(o) {
    opt_.target_rms  = positive_finite_or(opt_.target_rms, 0.2);
    opt_.attack      = positive_finite_or(opt_.attack, 0.01);
    opt_.release     = positive_finite_or(opt_.release, 0.20);
    opt_.max_gain    = positive_finite_or(opt_.max_gain, 64.0);
    opt_.min_gain    = positive_finite_or(opt_.min_gain, 1e-4);
    opt_.sample_rate = positive_finite_or(opt_.sample_rate, 48000.0);
    if (opt_.min_gain > opt_.max_gain) std::swap(opt_.min_gain, opt_.max_gain);

    alpha_attack_  = one_pole_alpha(opt_.attack, opt_.sample_rate);
    alpha_release_ = one_pole_alpha(opt_.release, opt_.sample_rate);

    // RMS 估计本身用一个固定的 10 ms 时间常数：它决定"多快看清当前功率"，
    // 而 attack/release 决定"增益多快跟过去"，两者职责不同。
    constexpr double kPowerTauSec = 0.01;
    beta_power_ = one_pole_alpha(kPowerTauSec, opt_.sample_rate);

    reset();
}

void Agc::process_inplace(Samples& x) {
    if (x.empty()) return;

    for (auto& sample : x) {
        const double v = static_cast<double>(sample);

        // 1) 滑动均方（RMS 的平方），初始为 target_rms^2，避免冷启动时
        //    把"还没估计出功率"误当成极弱信号而先把增益冲到 max。
        mean_sq_ += beta_power_ * (v * v - mean_sq_);
        const double rms = std::sqrt(std::max(mean_sq_, 1e-20));

        // 2) 目标增益：输出 RMS 要达到 target_rms；夹在 [min_gain, max_gain]。
        double desired = opt_.target_rms / rms;
        if (!std::isfinite(desired)) desired = opt_.max_gain;
        desired = std::min(std::max(desired, opt_.min_gain), opt_.max_gain);

        // 3) 一阶平滑：增益上升用 attack，下降用 release。
        const double alpha = (desired > gain_) ? alpha_attack_ : alpha_release_;
        gain_ += alpha * (desired - gain_);
        gain_ = std::min(std::max(gain_, opt_.min_gain), opt_.max_gain);

        sample = static_cast<float>(v * gain_);
    }
}

double Agc::gain() const noexcept { return gain_; }

void Agc::reset() noexcept {
    gain_    = 1.0;
    mean_sq_ = opt_.target_rms * opt_.target_rms;
}

}  // namespace my::dsp
