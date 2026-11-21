#include "my_cpp_libs/dsp/dsp.hpp"

#include <algorithm>
#include <cmath>

#include "common.hpp"

namespace my::dsp {
namespace {

using detail::kTwoPi;

double sinc(double x) {
    if (std::fabs(x) < 1e-12) return 1.0;
    const double pix = kTwoPi / 2.0 * x;  // pi * x
    return std::sin(pix) / pix;
}

bool valid_taps(std::size_t n) { return n >= 3 && (n % 2) == 1; }

/// 归一化：让 FIR 在指定频率处的幅度响应为 1
void normalize_at(Fir& fir, double freq_norm) {
    double re = 0.0;
    const std::size_t m = fir.taps.size();
    const double      d = static_cast<double>(m - 1) / 2.0;
    for (std::size_t i = 0; i < m; ++i) {
        const double t = static_cast<double>(i) - d;
        re += fir.taps[i] * std::cos(kTwoPi * freq_norm * t);
    }
    if (std::fabs(re) < 1e-9) return;
    for (auto& h : fir.taps) h = static_cast<float>(h / re);
}

}  // namespace

// -----------------------------------------------------------------------------
//  FIR
// -----------------------------------------------------------------------------
Result<Fir> fir_lowpass(double cutoff_hz, double sample_rate, std::size_t num_taps, Window window) {
    if (sample_rate <= 0.0) return Status(Errc::InvalidArgument, "采样率必须为正");
    if (!valid_taps(num_taps)) return Status(Errc::InvalidArgument, "抽头数必须是 >=3 的奇数");
    if (cutoff_hz <= 0.0 || cutoff_hz >= sample_rate / 2.0) {
        return Status(Errc::InvalidArgument, "截止频率必须在 (0, fs/2) 内");
    }

    const double      fc = cutoff_hz / sample_rate;  // 归一化到采样率
    const std::size_t m  = num_taps;
    const double      d  = static_cast<double>(m - 1) / 2.0;
    const auto        w  = make_window(window, m);

    Fir fir;
    fir.taps.resize(m);
    for (std::size_t i = 0; i < m; ++i) {
        const double t = static_cast<double>(i) - d;
        fir.taps[i]    = static_cast<float>(2.0 * fc * sinc(2.0 * fc * t) * w[i]);
    }
    normalize_at(fir, 0.0);  // 直流增益 1
    return fir;
}

Result<Fir> fir_highpass(double cutoff_hz, double sample_rate, std::size_t num_taps, Window window) {
    auto lp = fir_lowpass(cutoff_hz, sample_rate, num_taps, window);
    if (!lp) return lp.status();

    Fir fir = lp.value();
    for (auto& h : fir.taps) h = -h;                 // 谱反转：delta - lp
    fir.taps[(num_taps - 1) / 2] += 1.0f;
    return fir;
}

Result<Fir> fir_bandpass(double low_hz, double high_hz, double sample_rate, std::size_t num_taps,
                         Window window) {
    if (sample_rate <= 0.0) return Status(Errc::InvalidArgument, "采样率必须为正");
    if (!valid_taps(num_taps)) return Status(Errc::InvalidArgument, "抽头数必须是 >=3 的奇数");
    if (low_hz <= 0.0 || high_hz >= sample_rate / 2.0 || low_hz >= high_hz) {
        return Status(Errc::InvalidArgument, "通带必须满足 0 < lo < hi < fs/2");
    }

    const double      f1 = low_hz / sample_rate;
    const double      f2 = high_hz / sample_rate;
    const std::size_t m  = num_taps;
    const double      d  = static_cast<double>(m - 1) / 2.0;
    const auto        w  = make_window(window, m);

    Fir fir;
    fir.taps.resize(m);
    for (std::size_t i = 0; i < m; ++i) {
        const double t = static_cast<double>(i) - d;
        fir.taps[i]    = static_cast<float>(
            2.0 * f2 * sinc(2.0 * f2 * t) - 2.0 * f1 * sinc(2.0 * f1 * t)) *
            w[i];
    }
    normalize_at(fir, 0.5 * (f1 + f2));  // 中心频率增益 1
    return fir;
}

Result<Samples> fir_apply(const Fir& fir, const Samples& x) {
    if (fir.taps.empty()) return Status(Errc::InvalidArgument, "FIR 抽头为空");
    if (x.empty()) return Status(Errc::InvalidArgument, "输入为空");

    const std::size_t m     = fir.taps.size();
    const std::size_t delay = fir.delay();
    const std::size_t n     = x.size();
    Samples           y(n, 0.0f);

    // 直接卷积，输出与输入对齐（去掉群延迟）。O(N*M)，测试和短信号够用；
    // 长信号分块或 FFT 卷积留到后续（docs/08 记为待办）。
    for (std::size_t i = 0; i < n; ++i) {
        double acc = 0.0;
        for (std::size_t j = 0; j < m; ++j) {
            const std::ptrdiff_t idx = static_cast<std::ptrdiff_t>(i + delay) -
                                       static_cast<std::ptrdiff_t>(j);
            if (idx >= 0 && idx < static_cast<std::ptrdiff_t>(n)) {
                acc += static_cast<double>(fir.taps[j]) * x[static_cast<std::size_t>(idx)];
            }
        }
        y[i] = static_cast<float>(acc);
    }
    return y;
}

// -----------------------------------------------------------------------------
//  Biquad（RBJ cookbook）
// -----------------------------------------------------------------------------
Result<Biquad> biquad_design(BiquadType type, double freq_hz, double sample_rate, double q) {
    if (sample_rate <= 0.0) return Status(Errc::InvalidArgument, "采样率必须为正");
    if (freq_hz <= 0.0 || freq_hz >= sample_rate / 2.0) {
        return Status(Errc::InvalidArgument, "频率必须在 (0, fs/2) 内");
    }
    if (q <= 0.0) return Status(Errc::InvalidArgument, "q 必须为正");
    // 极低截止频率：极点会贴到 z=1，float 系数无法保证 1+a1+a2 的符号，
    // 直接拒绝比输出缓慢发散/NaN 更诚实（普通音频/基带截止远高于这个门限）。
    if (freq_hz < sample_rate * 1e-4) {
        return Status(Errc::OutOfRange,
                      "biquad 频率过低（< fs/10000），float 系数不稳定: " +
                          std::to_string(freq_hz) + " Hz @ " + std::to_string(sample_rate));
    }

    const double w0    = kTwoPi * freq_hz / sample_rate;
    const double cw    = std::cos(w0);
    const double sw    = std::sin(w0);
    const double alpha = sw / (2.0 * q);

    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a0 = 1.0 + alpha, a1 = -2.0 * cw, a2 = 1.0 - alpha;

    switch (type) {
        case BiquadType::Lowpass:
            b0 = (1.0 - cw) / 2.0;
            b1 = 1.0 - cw;
            b2 = (1.0 - cw) / 2.0;
            break;
        case BiquadType::Highpass:
            b0 = (1.0 + cw) / 2.0;
            b1 = -(1.0 + cw);
            b2 = (1.0 + cw) / 2.0;
            break;
        case BiquadType::Bandpass:  // 峰值增益 0 dB
            b0 = alpha;
            b1 = 0.0;
            b2 = -alpha;
            break;
        case BiquadType::Notch:
            b0 = 1.0;
            b1 = -2.0 * cw;
            b2 = 1.0;
            break;
    }

    Biquad bq;
    bq.b0 = static_cast<float>(b0 / a0);
    bq.b1 = static_cast<float>(b1 / a0);
    bq.b2 = static_cast<float>(b2 / a0);
    bq.a1 = static_cast<float>(a1 / a0);
    bq.a2 = static_cast<float>(a2 / a0);

    // 用**舍入后的 float 系数**复核二阶极点稳定性：
    //   |a2| < 1, 1+a1+a2 > 0, 1-a1+a2 > 0
    // 不检查的话，极低截止/极高 Q 会静默输出发散序列。
    const double a1f = bq.a1;
    const double a2f = bq.a2;
    if (!std::isfinite(a1f) || !std::isfinite(a2f) || std::fabs(a2f) >= 1.0 ||
        !(1.0 + a1f + a2f > 0.0) || !(1.0 - a1f + a2f > 0.0)) {
        return Status(Errc::OutOfRange,
                      "biquad 在 float 系数下不稳定（极点接近单位圆）: freq=" +
                          std::to_string(freq_hz) + " Hz, q=" + std::to_string(q));
    }
    return bq;
}

void biquad_apply(const Biquad& bq, Samples& x) {
    // 转置直接 II 型：只需要两个状态，数值上比直接 I 型稳
    float z1 = 0.0f;
    float z2 = 0.0f;
    for (auto& sample : x) {
        const float in  = sample;
        const float out = bq.b0 * in + z1;
        z1              = bq.b1 * in - bq.a1 * out + z2;
        z2              = bq.b2 * in - bq.a2 * out;
        sample          = out;
    }
}

// -----------------------------------------------------------------------------
//  混频 / 抽取
// -----------------------------------------------------------------------------
Result<CSamples> mix_down(const Samples& x, double freq_hz, double sample_rate) {
    if (x.empty()) return Status(Errc::InvalidArgument, "输入为空");
    if (sample_rate <= 0.0) return Status(Errc::InvalidArgument, "采样率必须为正");
    if (std::fabs(freq_hz) >= sample_rate / 2.0) {
        return Status(Errc::InvalidArgument, "混频频率必须在 (-fs/2, fs/2) 内");
    }

    const double dphi = -kTwoPi * freq_hz / sample_rate;
    CSamples     out(x.size());
    double       phase = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        out[i] = Complex(static_cast<float>(x[i] * std::cos(phase)),
                         static_cast<float>(x[i] * std::sin(phase)));
        phase += dphi;
    }
    return out;
}

Result<CSamples> mix_down(const CSamples& x, double freq_hz, double sample_rate) {
    if (x.empty()) return Status(Errc::InvalidArgument, "输入为空");
    if (sample_rate <= 0.0) return Status(Errc::InvalidArgument, "采样率必须为正");
    if (std::fabs(freq_hz) >= sample_rate / 2.0) {
        return Status(Errc::InvalidArgument, "混频频率必须在 (-fs/2, fs/2) 内");
    }

    const double dphi = -kTwoPi * freq_hz / sample_rate;
    CSamples     out(x.size());
    double       phase = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        out[i] = x[i] * Complex(static_cast<float>(std::cos(phase)),
                                static_cast<float>(std::sin(phase)));
        phase += dphi;
    }
    return out;
}

Result<Samples> decimate(const Samples& x, int factor, double sample_rate) {
    if (x.empty()) return Status(Errc::InvalidArgument, "输入为空");
    if (factor < 1) return Status(Errc::InvalidArgument, "factor 必须 >= 1");
    if (factor == 1) return x;

    const double new_nyquist = sample_rate / (2.0 * static_cast<double>(factor));
    // 抗混叠低通放在新奈奎斯特的 90% 处，留点过渡带
    auto lp = fir_lowpass(0.9 * new_nyquist, sample_rate,
                          static_cast<std::size_t>(16 * factor + 1), Window::Hamming);
    if (!lp) return lp.status();

    auto filtered = fir_apply(lp.value(), x);
    if (!filtered) return filtered.status();

    Samples out;
    out.reserve(filtered.value().size() / static_cast<std::size_t>(factor) + 1);
    for (std::size_t i = 0; i < filtered.value().size(); i += static_cast<std::size_t>(factor)) {
        out.push_back(filtered.value()[i]);
    }
    return out;
}

}  // namespace my::dsp
