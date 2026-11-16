// =============================================================================
//  my::dsp 单元测试
//
//  全部用**合成信号**：已知调制的波形 -> 预期的频谱/特征/分类/解调结果。
//  这样做的好处是"错了一定看得出来"，而不是拿一段真实数据跑出个漂亮但没法
//  验证的图。
//
//  覆盖：窗函数 / FFT / Welch / 峰值 / SNR / FIR / IIR / 混频 / 抽取 /
//        突发检测 / 特征 / 调制识别 / AM/FM/ASK/FSK/BPSK 解调。
// =============================================================================

#include "my_cpp_libs/dsp/dsp.hpp"

#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "test_framework.hpp"

MYTEST_SUITE("dsp");

namespace {

constexpr double kPi = 3.14159265358979323846;

using my::dsp::CSamples;
using my::dsp::Complex;
using my::dsp::Samples;

Samples sine(double freq_hz, double fs, std::size_t n, double amp = 1.0, double phase = 0.0) {
    Samples x(n);
    for (std::size_t i = 0; i < n; ++i) {
        x[i] = static_cast<float>(amp * std::sin(2.0 * kPi * freq_hz * static_cast<double>(i) / fs +
                                                 phase));
    }
    return x;
}

void add_awgn(Samples& x, double snr_db, std::uint32_t seed) {
    double power = 0.0;
    for (float v : x) power += static_cast<double>(v) * v;
    power /= static_cast<double>(x.size());
    const double sigma = std::sqrt(power / std::pow(10.0, snr_db / 10.0));

    std::mt19937                          rng(seed);
    std::normal_distribution<double>      dist(0.0, sigma);
    for (auto& v : x) v = static_cast<float>(static_cast<double>(v) + dist(rng));
}

/// 归一化互相关（衡量"解调出来的波形是不是那根正弦"）
double correlation(const Samples& a, const Samples& b) {
    const std::size_t n = std::min(a.size(), b.size());
    if (n == 0) return 0.0;
    double ma = 0.0, mb = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        ma += a[i];
        mb += b[i];
    }
    ma /= static_cast<double>(n);
    mb /= static_cast<double>(n);

    double num = 0.0, da = 0.0, db = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double va = a[i] - ma;
        const double vb = b[i] - mb;
        num += va * vb;
        da += va * va;
        db += vb * vb;
    }
    if (da <= 0.0 || db <= 0.0) return 0.0;
    return num / std::sqrt(da * db);
}

/// 比特错误率；BPSK 有 π 相位模糊，允许整体取反（取两者较小）
double ber(const std::vector<std::uint8_t>& got, const std::vector<std::uint8_t>& want) {
    const std::size_t n = std::min(got.size(), want.size());
    if (n == 0) return 1.0;
    std::size_t err = 0;
    std::size_t inv = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (got[i] != want[i]) ++err;
        if (got[i] == want[i]) ++inv;
    }
    return static_cast<double>(std::min(err, inv)) / static_cast<double>(n);
}

std::vector<std::uint8_t> random_bits(std::size_t n, std::uint32_t seed) {
    std::mt19937              rng(seed);
    std::bernoulli_distribution coin(0.5);
    std::vector<std::uint8_t> bits(n);
    for (auto& b : bits) b = coin(rng) ? 1u : 0u;
    return bits;
}

/// 2-FSK：相位连续（freq 变化时不重置相位，真实信号就是这样）
Samples fsk_signal(const std::vector<std::uint8_t>& bits, double fs, double symbol_rate,
                   double center_hz, double deviation_hz) {
    const std::size_t sps = static_cast<std::size_t>(fs / symbol_rate);
    Samples           x(bits.size() * sps);
    double            phase = 0.0;
    for (std::size_t k = 0; k < bits.size(); ++k) {
        const double f = center_hz + (bits[k] ? deviation_hz : -deviation_hz);
        for (std::size_t i = 0; i < sps; ++i) {
            x[k * sps + i] = static_cast<float>(std::sin(phase));
            phase += 2.0 * kPi * f / fs;
        }
    }
    return x;
}

/// OOK（ASK 的一个特例）
Samples ask_signal(const std::vector<std::uint8_t>& bits, double fs, double symbol_rate,
                   double carrier_hz) {
    const std::size_t sps = static_cast<std::size_t>(fs / symbol_rate);
    Samples           x(bits.size() * sps);
    for (std::size_t k = 0; k < bits.size(); ++k) {
        for (std::size_t i = 0; i < sps; ++i) {
            x[k * sps + i] = bits[k]
                                 ? static_cast<float>(std::sin(2.0 * kPi * carrier_hz *
                                                               static_cast<double>(k * sps + i) / fs))
                                 : 0.0f;
        }
    }
    return x;
}

}  // namespace

// -----------------------------------------------------------------------------
//  变换 / 频谱
// -----------------------------------------------------------------------------
MYTEST_CASE(dsp_window_properties) {
    const std::size_t n = 64;

    const auto rect = my::dsp::make_window(my::dsp::Window::Rect, n);
    CHECK_EQ(rect.size(), n);
    CHECK_NEAR(rect[0], 1.0, 1e-6);

    const auto hann = my::dsp::make_window(my::dsp::Window::Hann, n);
    CHECK_NEAR(hann[0], 0.0, 1e-6);
    CHECK_NEAR(hann[n - 1], 0.0, 1e-6);
    CHECK_NEAR(hann[n / 2], 1.0, 1e-3);  // 对称窗峰值在中点附近

    double sum = 0.0;
    for (float v : hann) sum += v;
    CHECK_NEAR(sum, static_cast<double>(n) / 2.0, 1.0);  // Hann 的相干增益 0.5
}

MYTEST_CASE(dsp_fft_single_tone) {
    const std::size_t n   = 64;
    const std::size_t bin = 8;

    CSamples x(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = 2.0 * kPi * static_cast<double>(bin * i) / static_cast<double>(n);
        x[i]            = Complex(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
    }

    CHECK_OK_VALUE(X, my::dsp::fft(x));
    CHECK_EQ(X.size(), n);

    std::size_t peak = 0;
    double      mag  = 0.0;
    for (std::size_t k = 0; k < n; ++k) {
        const double m = std::abs(X[k]);
        if (m > mag) {
            mag  = m;
            peak = k;
        }
    }
    CHECK_EQ(peak, bin);
    CHECK_NEAR(mag, static_cast<double>(n), 1e-2);  // 幅度 1 的复指数 -> |X| = N

    // ifft 不带 1/N 缩放：ifft(fft(x)) = N * x
    CHECK_OK_VALUE(back, my::dsp::ifft(X));
    CHECK_NEAR(back[3].real() / static_cast<double>(n), x[3].real(), 1e-5);
    CHECK_NEAR(back[3].imag() / static_cast<double>(n), x[3].imag(), 1e-5);
}

MYTEST_CASE(dsp_fft_zero_padding) {
    CSamples x(10, Complex(1.0f, 0.0f));
    CHECK_OK_VALUE(X, my::dsp::fft(x, 64));
    CHECK_EQ(X.size(), 64u);
    // 常数信号补零后，DC 分量 = 10
    CHECK_NEAR(std::abs(X[0]), 10.0, 1e-4);
}

MYTEST_CASE(dsp_welch_peak) {
    const double fs = 1000.0;
    const double f0 = 125.0;
    Samples      x  = sine(f0, fs, 8192);

    CHECK_OK_VALUE(s, my::dsp::welch(x, fs, 1024, 0.5f, my::dsp::Window::Hann));

    CHECK_EQ(s.sample_rate, fs);
    CHECK_EQ(s.size(), 1024u / 2 + 1);
    CHECK_NEAR(s.peak_freq, f0, static_cast<double>(fs) / static_cast<double>(s.nfft) * 1.5);
    CHECK_NEAR(s.peak_db, 0.0, 0.5);  // 满幅正弦 = 0 dBFS
    CHECK(s.noise_floor_db < -60.0f);  // 纯正弦的噪声底应当很低
    CHECK(my::dsp::estimate_snr_db(s) > 60.0f);
}

MYTEST_CASE(dsp_welch_noise_floor) {
    const double fs = 1000.0;
    Samples      x(8192);
    std::mt19937 rng(7);
    std::normal_distribution<float> dist(0.0f, 0.1f);
    for (auto& v : x) v = dist(rng);

    CHECK_OK_VALUE(s, my::dsp::welch(x, fs, 1024, 0.5f, my::dsp::Window::Hann));

    // 白噪声的谱应当比较平：各 bin 与中位数的偏差不大
    float max_dev = 0.0f;
    for (float db : s.db) max_dev = std::max(max_dev, std::fabs(db - s.noise_floor_db));
    CHECK(max_dev < 20.0f);
}

MYTEST_CASE(dsp_find_peaks) {
    const double fs = 2000.0;
    Samples      x(8192);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double t = static_cast<double>(i) / fs;
        x[i] = static_cast<float>(std::sin(2.0 * kPi * 100.0 * t) +
                                  0.5 * std::sin(2.0 * kPi * 400.0 * t) +
                                  0.25 * std::sin(2.0 * kPi * 700.0 * t));
    }

    CHECK_OK_VALUE(s, my::dsp::welch(x, fs, 1024, 0.5f));
    const auto peaks = my::dsp::find_peaks(s, 6.0f, 20.0, 8);
    CHECK(peaks.size() >= 3);

    bool has100 = false, has400 = false, has700 = false;
    for (const auto& p : peaks) {
        if (std::fabs(p.freq - 100.0) < 10.0) has100 = true;
        if (std::fabs(p.freq - 400.0) < 10.0) has400 = true;
        if (std::fabs(p.freq - 700.0) < 10.0) has700 = true;
    }
    CHECK(has100);
    CHECK(has400);
    CHECK(has700);
}

// -----------------------------------------------------------------------------
//  滤波
// -----------------------------------------------------------------------------
MYTEST_CASE(dsp_fir_lowpass_attenuates_high_tone) {
    const double fs = 1000.0;
    Samples      x(4096);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double t = static_cast<double>(i) / fs;
        x[i] = static_cast<float>(std::sin(2.0 * kPi * 50.0 * t) +
                                  std::sin(2.0 * kPi * 300.0 * t));
    }

    CHECK_OK_VALUE(lp, my::dsp::fir_lowpass(100.0, fs, 101, my::dsp::Window::Hamming));
    CHECK_OK_VALUE(y, my::dsp::fir_apply(lp, x));

    // 跳过两端（边缘效应），对两个频率分别做相关
    const std::size_t skip = 128;
    Samples           a(y.begin() + static_cast<std::ptrdiff_t>(skip),
                       y.end() - static_cast<std::ptrdiff_t>(skip));
    Samples           t(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        t[i] = static_cast<float>(std::sin(2.0 * kPi * 50.0 * static_cast<double>(i + skip) / fs));
    }
    const double c50 = correlation(a, t);
    for (std::size_t i = 0; i < a.size(); ++i) {
        t[i] = static_cast<float>(std::sin(2.0 * kPi * 300.0 * static_cast<double>(i + skip) / fs));
    }
    const double c300 = correlation(a, t);

    CHECK(c50 > 0.95);   // 通带保留
    CHECK(c300 < 0.10);  // 阻带压制
}

MYTEST_CASE(dsp_fir_highpass_removes_low_tone) {
    const double fs = 1000.0;
    Samples      x(4096);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double t = static_cast<double>(i) / fs;
        x[i] = static_cast<float>(std::sin(2.0 * kPi * 50.0 * t) +
                                  std::sin(2.0 * kPi * 300.0 * t));
    }

    CHECK_OK_VALUE(hp, my::dsp::fir_highpass(150.0, fs, 101, my::dsp::Window::Hamming));
    CHECK_OK_VALUE(y, my::dsp::fir_apply(hp, x));

    const std::size_t skip = 128;
    Samples           a(y.begin() + static_cast<std::ptrdiff_t>(skip),
                       y.end() - static_cast<std::ptrdiff_t>(skip));
    Samples           t(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        t[i] = static_cast<float>(std::sin(2.0 * kPi * 50.0 * static_cast<double>(i + skip) / fs));
    }
    const double c50 = correlation(a, t);
    for (std::size_t i = 0; i < a.size(); ++i) {
        t[i] = static_cast<float>(std::sin(2.0 * kPi * 300.0 * static_cast<double>(i + skip) / fs));
    }
    const double c300 = correlation(a, t);

    CHECK(c50 < 0.10);
    CHECK(c300 > 0.95);
}

MYTEST_CASE(dsp_fir_bandpass_keeps_mid_tone) {
    const double fs = 1000.0;
    Samples      x(4096);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double t = static_cast<double>(i) / fs;
        x[i] = static_cast<float>(std::sin(2.0 * kPi * 50.0 * t) +
                                  std::sin(2.0 * kPi * 300.0 * t) +
                                  std::sin(2.0 * kPi * 450.0 * t));
    }

    CHECK_OK_VALUE(bp, my::dsp::fir_bandpass(250.0, 350.0, fs, 201, my::dsp::Window::Hamming));
    CHECK_OK_VALUE(y, my::dsp::fir_apply(bp, x));

    const std::size_t skip = 200;
    Samples a(y.begin() + static_cast<std::ptrdiff_t>(skip),
              y.end() - static_cast<std::ptrdiff_t>(skip));
    auto tone_corr = [&](double hz) {
        Samples t(a.size());
        for (std::size_t i = 0; i < a.size(); ++i) {
            t[i] = static_cast<float>(
                std::sin(2.0 * kPi * hz * static_cast<double>(i + skip) / fs));
        }
        return correlation(a, t);
    };

    CHECK(tone_corr(300.0) > 0.95);   // 通带保留
    CHECK(tone_corr(50.0) < 0.10);    // 低频压制
    CHECK(tone_corr(450.0) < 0.10);   // 高频压制
}

MYTEST_CASE(dsp_modulation_name_covers_all) {
    using my::dsp::Modulation;
    CHECK(std::string(my::dsp::modulation_name(Modulation::Unknown)) == "unknown");
    CHECK(std::string(my::dsp::modulation_name(Modulation::Noise)) == "noise");
    CHECK(std::string(my::dsp::modulation_name(Modulation::CW)) == "CW");
    CHECK(std::string(my::dsp::modulation_name(Modulation::AM)) == "AM");
    CHECK(std::string(my::dsp::modulation_name(Modulation::ASK)) == "ASK");
    CHECK(std::string(my::dsp::modulation_name(Modulation::FM)) == "FM");
    CHECK(std::string(my::dsp::modulation_name(Modulation::FSK)) == "FSK");
    CHECK(std::string(my::dsp::modulation_name(Modulation::PSK)) == "PSK");
}

MYTEST_CASE(dsp_biquad_notch) {
    const double fs = 1000.0;
    Samples      x(4096);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double t = static_cast<double>(i) / fs;
        x[i] = static_cast<float>(std::sin(2.0 * kPi * 50.0 * t) +
                                  std::sin(2.0 * kPi * 200.0 * t));
    }

    CHECK_OK_VALUE(bq, my::dsp::biquad_design(my::dsp::BiquadType::Notch, 50.0, fs, 5.0));
    my::dsp::biquad_apply(bq, x);

    const std::size_t skip = 256;
    Samples           a(x.begin() + static_cast<std::ptrdiff_t>(skip),
                       x.end() - static_cast<std::ptrdiff_t>(skip));
    Samples           t(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        t[i] = static_cast<float>(std::sin(2.0 * kPi * 50.0 * static_cast<double>(i + skip) / fs));
    }
    const double c50 = correlation(a, t);
    for (std::size_t i = 0; i < a.size(); ++i) {
        t[i] = static_cast<float>(std::sin(2.0 * kPi * 200.0 * static_cast<double>(i + skip) / fs));
    }
    const double c200 = correlation(a, t);

    CHECK(c50 < 0.20);  // 50 Hz 被陷掉
    CHECK(c200 > 0.90);
}

// -----------------------------------------------------------------------------
//  混频 / 抽取
// -----------------------------------------------------------------------------
MYTEST_CASE(dsp_mix_down_moves_tone_to_dc) {
    const double fs = 1000.0;
    const double f0 = 200.0;

    // 复信号版本：干净的搬移（实信号混频会留下 -2f 的镜像，见下面的检查）
    CSamples xc(2048);
    for (std::size_t i = 0; i < xc.size(); ++i) {
        const double ph = 2.0 * kPi * f0 * static_cast<double>(i) / fs;
        xc[i] = Complex(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
    }
    CHECK_OK_VALUE(y, my::dsp::mix_down(xc, f0, fs));
    CHECK_EQ(y.size(), xc.size());

    double max_step = 0.0;
    for (std::size_t i = 1; i < y.size(); ++i) {
        max_step = std::max(max_step,
                            static_cast<double>(std::fabs(std::arg(y[i] * std::conj(y[i - 1])))));
    }
    CHECK(max_step < 0.01);
    double amp = 0.0;
    for (std::size_t i = 100; i < y.size() - 100; ++i) amp += std::abs(y[i]);
    amp /= static_cast<double>(y.size() - 200);
    CHECK_NEAR(amp, 1.0, 0.05);

    // 实信号版本：下变频后 DC 分量的幅度是 A/2（实信号的正频率分量），
    // 同时会留下 -2f 的镜像 —— 这是实信号混频的固有性质，不是 bug。
    const Samples xr = sine(f0, fs, 2048);
    CHECK_OK_VALUE(yr, my::dsp::mix_down(xr, f0, fs));
    std::complex<double> sum(0.0, 0.0);
    for (std::size_t i = 100; i < 1100; ++i) {
        sum += std::complex<double>(yr[i].real(), yr[i].imag());
    }
    CHECK(std::abs(sum) / 1000.0 > 0.3);  // 期望 ~0.5
}

MYTEST_CASE(dsp_decimate_preserves_inband_tone) {
    const double fs = 1000.0;
    Samples      x(8192);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double t = static_cast<double>(i) / fs;
        x[i] = static_cast<float>(std::sin(2.0 * kPi * 50.0 * t) +
                                  0.5 * std::sin(2.0 * kPi * 400.0 * t));
    }

    CHECK_OK_VALUE(y, my::dsp::decimate(x, 4, fs));
    const double fs_new = fs / 4.0;
    CHECK(y.size() >= x.size() / 4 - 2);

    // 50 Hz 在新采样率下仍是 50 Hz；400 Hz 被抗混叠滤波器压掉
    const std::size_t skip = 64;
    Samples           a(y.begin() + static_cast<std::ptrdiff_t>(skip),
                       y.end() - static_cast<std::ptrdiff_t>(skip));
    Samples           t(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        t[i] = static_cast<float>(std::sin(2.0 * kPi * 50.0 * static_cast<double>(i + skip) / fs_new));
    }
    CHECK(correlation(a, t) > 0.95);
}

// -----------------------------------------------------------------------------
//  检测
// -----------------------------------------------------------------------------
MYTEST_CASE(dsp_detect_two_bursts) {
    const double      fs = 1000.0;
    const std::size_t n  = 6000;
    Samples           x(n, 0.0f);

    const std::size_t b1 = 1000, b2 = 3500, len = 500;
    for (std::size_t i = 0; i < len; ++i) {
        x[b1 + i] = static_cast<float>(std::sin(2.0 * kPi * 100.0 * static_cast<double>(i) / fs));
        x[b2 + i] = static_cast<float>(std::sin(2.0 * kPi * 100.0 * static_cast<double>(i) / fs));
    }

    const auto bursts = my::dsp::detect_bursts(x, 64, 10.0f, 0);
    CHECK_EQ(bursts.size(), 2u);
    if (bursts.size() == 2) {
        CHECK(bursts[0].begin <= b1 + 64);
        CHECK(bursts[0].end >= b1 + len - 64);
        CHECK(bursts[1].begin <= b2 + 64);
        CHECK(bursts[1].end >= b2 + len - 64);
    }
}

// -----------------------------------------------------------------------------
//  特征 / 识别
// -----------------------------------------------------------------------------
MYTEST_CASE(dsp_analyze_cw) {
    const double fs = 2000.0;
    Samples      x  = sine(300.0, fs, 8192);
    add_awgn(x, 40.0, 11);

    CHECK_OK_VALUE(f, my::dsp::analyze(x, fs));
    CHECK(f.envelope_cv < 0.05);
    CHECK(f.inst_freq_std < 0.01);
    CHECK_NEAR(f.center_freq_hz, 300.0, 20.0);

    const auto c = my::dsp::classify(f);
    CHECK_EQ(static_cast<int>(c.modulation), static_cast<int>(my::dsp::Modulation::CW));
}

MYTEST_CASE(dsp_analyze_am) {
    const double fs = 4000.0;
    const double fc = 800.0, fm = 50.0, m = 0.8;
    Samples      x(16384);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double t = static_cast<double>(i) / fs;
        x[i] = static_cast<float>((1.0 + m * std::sin(2.0 * kPi * fm * t)) *
                                  std::sin(2.0 * kPi * fc * t));
    }
    add_awgn(x, 40.0, 12);

    CHECK_OK_VALUE(f, my::dsp::analyze(x, fs));
    CHECK(f.envelope_cv > 0.30);
    CHECK(f.envelope_low_frac < 0.30);

    const auto c = my::dsp::classify(f);
    CHECK_EQ(static_cast<int>(c.modulation), static_cast<int>(my::dsp::Modulation::AM));
}

MYTEST_CASE(dsp_analyze_ask) {
    const double fs = 4000.0;
    const double carrier = 800.0, symbol_rate = 200.0;
    const auto   bits = random_bits(40, 13);
    Samples      x    = ask_signal(bits, fs, symbol_rate, carrier);
    add_awgn(x, 40.0, 14);

    CHECK_OK_VALUE(f, my::dsp::analyze(x, fs));
    CHECK(f.envelope_cv > 0.30);
    CHECK(f.envelope_low_frac > 0.30);
    CHECK_NEAR(f.center_freq_hz, carrier, 60.0);

    const auto c = my::dsp::classify(f);
    CHECK_EQ(static_cast<int>(c.modulation), static_cast<int>(my::dsp::Modulation::ASK));
}

MYTEST_CASE(dsp_analyze_fm) {
    const double fs = 8000.0;
    const double fc = 1000.0, fm = 100.0, beta = 2.0;
    Samples      x(32768);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double t = static_cast<double>(i) / fs;
        x[i] = static_cast<float>(
            std::sin(2.0 * kPi * fc * t + beta * std::sin(2.0 * kPi * fm * t)));
    }
    add_awgn(x, 40.0, 15);

    CHECK_OK_VALUE(f, my::dsp::analyze(x, fs));
    CHECK(f.envelope_cv < 0.10);
    CHECK(f.inst_freq_std > 0.02);
    CHECK(f.inst_freq_quiet < 0.60);

    const auto c = my::dsp::classify(f);
    CHECK_EQ(static_cast<int>(c.modulation), static_cast<int>(my::dsp::Modulation::FM));
}

MYTEST_CASE(dsp_analyze_fsk) {
    const double fs = 4000.0;
    const auto   bits = random_bits(60, 16);
    Samples      x    = fsk_signal(bits, fs, 200.0, 800.0, 50.0);
    add_awgn(x, 40.0, 17);

    CHECK_OK_VALUE(f, my::dsp::analyze(x, fs));
    CHECK(f.envelope_cv < 0.10);
    CHECK(f.inst_freq_two_level > 0.60);

    const auto c = my::dsp::classify(f);
    CHECK_EQ(static_cast<int>(c.modulation), static_cast<int>(my::dsp::Modulation::FSK));
}

MYTEST_CASE(dsp_analyze_noise) {
    const double fs = 4000.0;
    Samples      x(16384);
    std::mt19937 rng(18);
    std::normal_distribution<float> dist(0.0f, 0.3f);
    for (auto& v : x) v = dist(rng);

    CHECK_OK_VALUE(f, my::dsp::analyze(x, fs));
    CHECK(f.spectral_flatness > 0.30);

    const auto c = my::dsp::classify(f);
    CHECK_EQ(static_cast<int>(c.modulation), static_cast<int>(my::dsp::Modulation::Noise));
}

// -----------------------------------------------------------------------------
//  解调
// -----------------------------------------------------------------------------
MYTEST_CASE(dsp_demod_am_recovers_modulation) {
    const double fs = 4000.0;
    const double fc = 800.0, fm = 50.0, m = 0.5;
    Samples      x(16384);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double t = static_cast<double>(i) / fs;
        x[i] = static_cast<float>((1.0 + m * std::sin(2.0 * kPi * fm * t)) *
                                  std::sin(2.0 * kPi * fc * t));
    }

    CHECK_OK_VALUE(y, my::dsp::demod_am(x, fs, 200.0));
    CHECK_EQ(y.size(), x.size());

    const std::size_t skip = 512;
    Samples           a(y.begin() + static_cast<std::ptrdiff_t>(skip),
                       y.end() - static_cast<std::ptrdiff_t>(skip));
    Samples           t(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        t[i] = static_cast<float>(
            std::sin(2.0 * kPi * fm * static_cast<double>(i + skip) / fs));
    }
    CHECK(correlation(a, t) > 0.90);
}

MYTEST_CASE(dsp_demod_fm_recovers_modulation) {
    const double fs = 8000.0;
    const double fc = 1000.0, fm = 100.0, beta = 2.0;
    Samples      x(32768);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double t = static_cast<double>(i) / fs;
        x[i] = static_cast<float>(
            std::sin(2.0 * kPi * fc * t + beta * std::sin(2.0 * kPi * fm * t)));
    }

    CHECK_OK_VALUE(y, my::dsp::demod_fm(x, fs, 500.0));
    const std::size_t skip = 1024;
    Samples           a(y.begin() + static_cast<std::ptrdiff_t>(skip),
                       y.end() - static_cast<std::ptrdiff_t>(skip));
    Samples           t(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        t[i] = static_cast<float>(
            std::cos(2.0 * kPi * fm * static_cast<double>(i + skip) / fs));  // 鉴频给出的是导数
    }
    CHECK(correlation(a, t) > 0.90);
}

MYTEST_CASE(dsp_demod_ask_bits) {
    const double fs = 4000.0;
    const double carrier = 800.0, symbol_rate = 200.0;
    const auto   bits = random_bits(48, 21);
    Samples      x    = ask_signal(bits, fs, symbol_rate, carrier);
    add_awgn(x, 30.0, 22);

    CHECK_OK_VALUE(got, my::dsp::demod_ask(x, fs, symbol_rate));
    CHECK(got.size() >= bits.size() - 1);
    CHECK(ber(got, bits) < 0.02);
}

MYTEST_CASE(dsp_demod_fsk_bits) {
    const double fs = 4000.0;
    const auto   bits = random_bits(60, 23);
    Samples      x    = fsk_signal(bits, fs, 200.0, 800.0, 50.0);
    add_awgn(x, 30.0, 24);

    CHECK_OK_VALUE(got, my::dsp::demod_fsk(x, fs, 200.0, 850.0, 750.0));
    CHECK(got.size() >= bits.size() - 1);
    CHECK(ber(got, bits) < 0.02);
}

MYTEST_CASE(dsp_demod_bpsk_bits_with_cfo) {
    const double fs  = 4000.0;
    const double rs  = 200.0;   // 每符号 20 个采样
    const double cfo = 3.0;     // Hz，落在平方环能估的范围内
    const double theta = 0.7;   // 任意载波相位

    const auto bits = random_bits(80, 25);
    const auto sps  = static_cast<std::size_t>(fs / rs);

    CSamples x(bits.size() * sps);
    for (std::size_t k = 0; k < bits.size(); ++k) {
        const float sym = bits[k] ? 1.0f : -1.0f;
        for (std::size_t i = 0; i < sps; ++i) {
            const double n = static_cast<double>(k * sps + i);
            const double ph = 2.0 * kPi * cfo * n / fs + theta;
            x[k * sps + i] = Complex(sym * static_cast<float>(std::cos(ph)),
                                     sym * static_cast<float>(std::sin(ph)));
        }
    }
    // 复高斯噪声
    {
        std::mt19937 rng(26);
        std::normal_distribution<double> dist(0.0, 0.05);
        for (auto& v : x) v += Complex(static_cast<float>(dist(rng)), static_cast<float>(dist(rng)));
    }

    CHECK_OK_VALUE(got, my::dsp::demod_bpsk(x, fs, rs));
    CHECK(got.size() >= bits.size() - 2);
    CHECK(ber(got, bits) < 0.02);
}

MYTEST_CASE(dsp_slice_bits) {
    Samples soft = {0.0f, 0.49f, 0.51f, 1.0f};
    const auto bits = my::dsp::slice_bits(soft);
    CHECK_EQ(bits.size(), 4u);
    CHECK_EQ(static_cast<int>(bits[0]), 0);
    CHECK_EQ(static_cast<int>(bits[1]), 0);
    CHECK_EQ(static_cast<int>(bits[2]), 1);
    CHECK_EQ(static_cast<int>(bits[3]), 1);
}

MYTEST_CASE(dsp_invalid_arguments) {
    const my::dsp::Samples empty;
    CHECK_ERR(my::dsp::welch(empty, 1000.0, 256));
    CHECK_ERR(my::dsp::welch(my::dsp::Samples(100, 0.0f), 1000.0, 256));  // 长度不足
    CHECK_ERR(my::dsp::fir_lowpass(100.0, 1000.0, 100));                  // 偶数抽头
    CHECK_ERR(my::dsp::fir_lowpass(600.0, 1000.0, 101));                  // 超奈奎斯特
    CHECK_ERR(my::dsp::biquad_design(my::dsp::BiquadType::Lowpass, 0.0, 1000.0));
    CHECK_ERR(my::dsp::decimate(my::dsp::Samples(100, 0.0f), 0, 1000.0));
    CHECK_ERR(my::dsp::demod_bpsk(CSamples(10, Complex(1, 0)), 1000.0, 200.0));  // 样本太少
}
