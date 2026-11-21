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
#include <limits>
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

/// 手工构造一条"平坦噪声底 + 若干尖峰"的谱。
/// CFAR 用例要精确控制尖峰落在哪个 bin、幅度多少，走 Welch 反而不好构造。
my::dsp::Spectrum flat_spectrum(std::size_t n, float floor_db) {
    my::dsp::Spectrum s;
    s.sample_rate = 6400.0;
    s.nfft        = n;
    s.freq_hz.resize(n);
    s.db.assign(n, floor_db);
    for (std::size_t k = 0; k < n; ++k) {
        s.freq_hz[k] = static_cast<float>(static_cast<double>(k) * s.sample_rate /
                                          static_cast<double>(n));
    }
    s.noise_floor_db = floor_db;
    s.peak_db        = floor_db;
    return s;
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

/// 比特错误率。
///
/// `allow_invert` 只给 **BPSK** 用：它有 π 相位模糊，判决可能整体取反，这时算"取反后"较小值
/// 是合理的。ASK/FSK 没有这个模糊 —— 以前的默认实现把取反也算成正确，于是
/// "0/1 判反了"这种真 bug 会被 BER=0 掩掉。默认必须是不允许取反。
double ber(const std::vector<std::uint8_t>& got, const std::vector<std::uint8_t>& want,
           bool allow_invert = false) {
    const std::size_t n = std::min(got.size(), want.size());
    if (n == 0) return 1.0;
    std::size_t err = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if ((got[i] != 0) != (want[i] != 0)) ++err;
    }
    double rate = static_cast<double>(err) / static_cast<double>(n);
    if (allow_invert) rate = std::min(rate, 1.0 - rate);
    return rate;
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

// 随机复序列的完整往返。dsp_fft_single_tone 只拿单音比了一个样本点，
// 这里逐点比对，同时锁住"ifft 不除 N"的约定（所以下面要显式除以 n）。
MYTEST_CASE(dsp_fft_ifft_roundtrip) {
    const std::size_t n = 128;
    std::mt19937                          rng(4242);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    CSamples x(n);
    for (auto& v : x) v = Complex(dist(rng), dist(rng));

    CHECK_OK_VALUE(X, my::dsp::fft(x));
    CHECK_EQ(X.size(), n);
    CHECK_OK_VALUE(y, my::dsp::ifft(X));
    CHECK_EQ(y.size(), n);

    double max_err = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::complex<double> got = std::complex<double>(y[i]) / static_cast<double>(n);
        const std::complex<double> want(x[i]);
        max_err = std::max(max_err, std::abs(got - want));
    }
    CHECK(max_err < 1e-4);  // 单精度往返误差应当在 1e-6 量级
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

// find_peaks 的契约（0.1.4 起）：
//   * 局部极大；
//   * 高于 noise_floor + min_prominence（粗筛）；
//   * **真·prominence >= min_prominence**（标准定义，见 dsp.hpp）；
//   * 输出按频率升序；max_peaks 截断保留最强的。
//
// 这条用例是"定语义"的直接证据：单音谱必须只返回**一个**峰。
// 旧实现在这里会返回 8 个（7 个是噪声起伏，peaks.front() 甚至是 320 Hz）。
MYTEST_CASE(dsp_find_peaks_tone_contract) {
    const double fs = 8000.0;
    const double f0 = 1000.0;
    Samples      x  = sine(f0, fs, 1024);

    CHECK_OK_VALUE(s, my::dsp::welch(x, fs, 1024, 0.5f, my::dsp::Window::Hann));
    CHECK_EQ(s.size(), std::size_t{1024 / 2 + 1});
    // 1000 Hz 在 fs=8k、nfft=1024 下正好落在 bin 128
    CHECK_EQ(s.peak_bin, std::size_t{128});
    CHECK_NEAR(s.peak_freq, 1000.0, 1.0);
    CHECK_NEAR(s.peak_db, 0.0, 0.5);  // 满幅单音 = 0 dBFS

    const auto peaks = my::dsp::find_peaks(s);  // 默认 min_prominence_db = 6
    CHECK_EQ(peaks.size(), std::size_t{1});     // 干净单音只该有一个峰
    CHECK_EQ(peaks[0].bin, s.peak_bin);
    CHECK_NEAR(peaks[0].freq, 1000.0, 1.0);
    CHECK_NEAR(peaks[0].db, 0.0, 0.5);
    // 单音两侧都没有更高的 bin，所以 prominence = 峰顶 - 整段最低谷底。
    // 注意 noise_floor_db 是**中位数**，一般高于最低谷底，所以这里是 >= 而不是 ==。
    CHECK(peaks[0].prominence_db > 100.0f);
    CHECK(peaks[0].prominence_db >= s.peak_db - s.noise_floor_db);
}

MYTEST_CASE(dsp_find_peaks_two_tones) {
    const double fs = 8000.0;
    Samples      x(4096);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double tt = static_cast<double>(i) / fs;
        x[i] = static_cast<float>(std::sin(2.0 * kPi * 1000.0 * tt) +
                                  0.3 * std::sin(2.0 * kPi * 2500.0 * tt));
    }

    CHECK_OK_VALUE(s, my::dsp::welch(x, fs, 1024, 0.5f));
    const auto peaks = my::dsp::find_peaks(s);
    CHECK_EQ(peaks.size(), std::size_t{2});
    // 调幅差 0.3 ≈ -10.5 dB，两峰都该显著高于噪声底
    CHECK_NEAR(peaks[0].freq, 1000.0, 8.0);
    CHECK_NEAR(peaks[1].freq, 2500.0, 8.0);
    CHECK(peaks[0].freq < peaks[1].freq);        // 频率升序
    CHECK(peaks[0].db > peaks[1].db);            // 但 1 kHz 更强
    CHECK(peaks[1].prominence_db > 40.0f);
}

MYTEST_CASE(dsp_find_peaks_noise_only) {
    // 纯噪声：没有真·prominence 超过 6 dB 的起伏，应当一个峰都不返回。
    // （旧实现会返回 max_peaks 个噪声 bin —— 这就是这次改语义要解决的问题。）
    const double fs = 8000.0;
    Samples      x(4096);
    std::mt19937 rng(2024);
    std::normal_distribution<float> dist(0.0f, 0.05f);
    for (auto& v : x) v = dist(rng);

    CHECK_OK_VALUE(s, my::dsp::welch(x, fs, 1024, 0.5f));
    const auto peaks = my::dsp::find_peaks(s, 6.0f);
    CHECK(peaks.empty());
    // 放宽门限到 2 dB 之后才会开始有大把噪声峰 —— 说明门限是真在起作用
    const auto loose = my::dsp::find_peaks(s, 2.0f);
    CHECK(loose.size() > peaks.size());
}

MYTEST_CASE(dsp_find_peaks_max_keeps_strongest) {
    // 四个音，幅度 1.0 / 0.5 / 0.25 / 0.125：max_peaks=2 时必须保留**最强的两个**
    // （1 kHz 与 1.5 kHz），而不是频率最低的那两个。
    const double fs = 8000.0;
    Samples      x(4096);
    const double freqs[4] = {700.0, 1000.0, 1500.0, 2700.0};
    const double amps[4]  = {0.125, 1.0, 0.5, 0.25};
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double tt = static_cast<double>(i) / fs;
        double       v  = 0.0;
        for (int k = 0; k < 4; ++k) v += amps[k] * std::sin(2.0 * kPi * freqs[k] * tt);
        x[i] = static_cast<float>(v);
    }

    CHECK_OK_VALUE(s, my::dsp::welch(x, fs, 1024, 0.5f));
    const auto peaks = my::dsp::find_peaks(s, 6.0f, 0.0, 2);
    CHECK_EQ(peaks.size(), std::size_t{2});
    CHECK_NEAR(peaks[0].freq, 1000.0, 8.0);
    CHECK_NEAR(peaks[1].freq, 1500.0, 8.0);
}

MYTEST_CASE(dsp_find_peaks_min_sep_merges) {
    // 两个相隔 8 Hz 的音 + min_sep=50 Hz：只能留下更强的那一个。
    const double fs = 4000.0;
    // 32768 点 = 16 段 Welch 平均（nfft=4096、50% 重叠）：真实系统的谱都是平均过的，
    // 平均段数少的时候单个噪声 bin 也能比中位数高 6 dB（见下面 noise_gate 那条用例）。
    Samples      x(32768);
    const double f1 = 500.0, f2 = 508.0;
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0f, 1e-3f);  // -60 dB 底噪，贴近真实采集
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double tt = static_cast<double>(i) / fs;
        x[i] = static_cast<float>(std::sin(2.0 * kPi * f1 * tt) +
                                  0.6 * std::sin(2.0 * kPi * f2 * tt)) +
               noise(rng);
    }

    CHECK_OK_VALUE(s, my::dsp::welch(x, fs, 4096, 0.5f));

    const auto merged = my::dsp::find_peaks(s, 6.0f, 50.0, 8);

    CHECK_EQ(merged.size(), std::size_t{1});
    CHECK_NEAR(merged[0].freq, 500.0, 10.0);  // 更强的是 500 Hz

    const auto both = my::dsp::find_peaks(s, 6.0f, 0.0, 8);
    CHECK(both.size() >= 2);  // 不限间隔时两个都在
}
// find_peaks 的**已知限制**（用测试钉住，防止有人"顺手修"错）：
// min_prominence_db 是"相对噪声底"的门限，而 Welch 平均段数很少时，单个噪声 bin
// 本来就能比中位数高 6 dB 以上 —— 这时 6 dB 的默认门限会放进几个噪声峰。
// 这不是 bug，是判据本身的性质：要更干净就调高门限（或按 Peak::db 再过滤）。
// 真正抗噪的检测器需要 CFAR（docs/08 §8 的待办项）。
MYTEST_CASE(dsp_find_peaks_noise_gate_limitation) {
    const double fs = 4000.0;
    Samples      x(8192);   // nfft=4096 -> 只有 3 段平均，噪声方差大
    std::mt19937 rng(11);
    std::normal_distribution<float> noise(0.0f, 1e-3f);
    for (auto& v : x) v = noise(rng);

    CHECK_OK_VALUE(s, my::dsp::welch(x, fs, 4096, 0.5f));

    // 默认 6 dB：会放进若干噪声峰（这是这个门限的性质，见用例标题）
    const auto loose = my::dsp::find_peaks(s, 6.0f);
    CHECK(!loose.empty());

    // 12 dB：噪声峰被挡住，返回空
    const auto strict = my::dsp::find_peaks(s, 12.0f);
    CHECK(strict.empty());

    // 但 12 dB 不会把真正的音挡掉：加一个 -30 dB 的音，strict 门限仍能找到它
    Samples y(8192);
    for (std::size_t i = 0; i < y.size(); ++i) {
        const double tt = static_cast<double>(i) / fs;
        y[i] = static_cast<float>(0.03 * std::sin(2.0 * kPi * 700.0 * tt)) + noise(rng);
    }
    CHECK_OK_VALUE(s2, my::dsp::welch(y, fs, 4096, 0.5f));
    const auto with_tone = my::dsp::find_peaks(s2, 12.0f);
    CHECK_EQ(with_tone.size(), std::size_t{1});
    CHECK_NEAR(with_tone[0].freq, 700.0, 10.0);
}

// -----------------------------------------------------------------------------
//  CFAR
// -----------------------------------------------------------------------------
MYTEST_CASE(dsp_cfar_ca_alpha_formula) {
    // alpha = N*(pfa^(-1/N)-1)，N = 2*train_cells
    const double expected = 16.0 * (std::pow(1e-4, -1.0 / 16.0) - 1.0);
    const double alpha    = my::dsp::cfar_ca_alpha(8, 1e-4);
    CHECK_NEAR(alpha, expected, 1e-9 * expected);  // 相对 1e-9
    CHECK_NEAR(alpha, 12.45247056, 1e-6);          // 手算值
    CHECK_NEAR(10.0 * std::log10(alpha), 10.95, 0.01);  // 约 11 dB

    // 虚警率放宽 -> 门限系数变小（门限更低，检出更多）
    const double loose = my::dsp::cfar_ca_alpha(8, 1e-2);
    CHECK(loose < alpha);
    CHECK_NEAR(loose, 16.0 * (std::pow(1e-2, -1.0 / 16.0) - 1.0), 1e-9 * loose);
}

MYTEST_CASE(dsp_cfar_ca_finds_tone) {
    const double fs = 8000.0;
    const double f0 = 1000.0;
    Samples      x  = sine(f0, fs, 4096);
    add_awgn(x, 20.0, 42);

    CHECK_OK_VALUE(s, my::dsp::welch(x, fs, 1024, 0.5f, my::dsp::Window::Hann));
    const auto det = my::dsp::ca_cfar(s);

    const my::dsp::Detection* tone = nullptr;
    for (const auto& d : det) {
        if (std::fabs(d.freq - f0) <= 20.0) tone = &d;
    }
    CHECK(tone != nullptr);              // 1 kHz 附近的峰必须被检出
    CHECK_EQ(tone->bin, s.peak_bin);
    CHECK_NEAR(tone->db, 0.0, 1.0);      // 满幅单音 + 20 dB SNR，峰约 0 dBFS
    CHECK(tone->threshold_db < tone->db - 10.0f);  // 门限明显低于峰
    CHECK(tone->snr_db > 10.0f);
}

MYTEST_CASE(dsp_cfar_ca_noise_only_low_pfa) {
    // 纯噪声谱：pfa 应当真的在控制虚警数量，而不是摆设。
    // 用 nfft=4096（8192 点 -> 只有 3 段平均）复现 docs/08 §10.4 说的"段数少"场景：
    // 此时噪声 bin 的起伏才足以让 pfa=0.1 产生可观虚警。
    const double fs = 8000.0;
    Samples      x(8192);
    std::mt19937 rng(2025);
    std::normal_distribution<float> dist(0.0f, 0.1f);
    for (auto& v : x) v = dist(rng);

    CHECK_OK_VALUE(s, my::dsp::welch(x, fs, 4096, 0.5f, my::dsp::Window::Hann));

    my::dsp::CfarOptions strict;
    strict.pfa = 1e-4;
    const auto low = my::dsp::ca_cfar(s, strict);
    CHECK(low.size() <= 2u);  // 期望虚警 ~1e-4 × ~2029 个可用 bin

    my::dsp::CfarOptions loose;
    loose.pfa = 0.1;
    const auto high = my::dsp::ca_cfar(s, loose);
    CHECK(high.size() > low.size());
    CHECK(high.size() >= 10u);  // 10% 虚警率在 ~2029 个 bin 上应给出可观数量
}

MYTEST_CASE(dsp_cfar_os_beats_ca_next_to_strong_interferer) {
    // 弱音（-40 dB）紧邻强音（0 dB，间隔 4 个 bin）。
    // CA 的参考窗把强音平均进来 -> 门限被抬到 0 dB 以上，弱音漏检；
    // OS 取第 12 小的参考单元，被强音污染的大值排不到第 12 位，仍能检出弱音。
    // 噪声取 3e-3：让局部噪声估计压过强音在 2 个 bin 外的窗泄漏，避免两个超门限
    // 区段连成一片（那样合并后只剩强音那个检测，看不出 OS 的优势）。
    const double fs       = 8000.0;
    const double f_strong = 1000.0;
    const double f_weak   = 1031.25;  // nfft=1024 时 4 个 bin = 31.25 Hz

    Samples      x(4096);
    std::mt19937 rng(31);
    std::normal_distribution<float> noise(0.0f, 3e-3f);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double t = static_cast<double>(i) / fs;
        x[i] = static_cast<float>(std::sin(2.0 * kPi * f_strong * t) +
                                  0.01 * std::sin(2.0 * kPi * f_weak * t)) +
               noise(rng);
    }

    CHECK_OK_VALUE(s, my::dsp::welch(x, fs, 1024, 0.5f, my::dsp::Window::Hann));

    const auto ca = my::dsp::ca_cfar(s);
    const auto os = my::dsp::os_cfar(s);

    auto has_near = [](const std::vector<my::dsp::Detection>& v, double hz, double tol) {
        for (const auto& d : v) {
            if (std::fabs(d.freq - hz) <= tol) return true;
        }
        return false;
    };

    CHECK(has_near(ca, f_strong, 20.0));  // 强音两者都能检出
    CHECK(!has_near(ca, f_weak, 20.0));   // CA：参考窗吃进强音 -> 弱音漏检
    CHECK(has_near(os, f_weak, 20.0));    // OS：rank 统计忽略强音 -> 仍能检出

    const my::dsp::Detection* weak = nullptr;
    for (const auto& d : os) {
        if (std::fabs(d.freq - f_weak) <= 20.0) weak = &d;
    }
    CHECK(weak != nullptr);
    CHECK_NEAR(weak->db, -40.0, 3.0);
    CHECK(weak->threshold_db < weak->db - 10.0f);
    CHECK(weak->snr_db > 10.0f);
}

MYTEST_CASE(dsp_cfar_edges_skipped) {
    // 两端各 guard+train = 10 个 bin 没有完整参考窗，直接不参与检测。
    const std::size_t n = 64;
    my::dsp::Spectrum s = flat_spectrum(n, -100.0f);
    s.db[5]             = -10.0f;  // bin < 10：边缘，不检测
    s.db[30]            = -10.0f;  // 中间：检测
    s.db[60]            = -10.0f;  // bin > 64-1-10：边缘，不检测

    const auto det = my::dsp::ca_cfar(s);
    CHECK_EQ(det.size(), std::size_t{1});
    CHECK_EQ(det[0].bin, std::size_t{30});
    CHECK_NEAR(det[0].freq, 3000.0, 1e-3);  // 6400 Hz / 64 bin = 100 Hz/bin
    CHECK_NEAR(det[0].threshold_db, -89.05, 0.05);  // -100 + 10log10(12.4525)
    CHECK_NEAR(det[0].snr_db, 90.0, 0.05);
}

MYTEST_CASE(dsp_cfar_invalid_options) {
    my::dsp::Spectrum s = flat_spectrum(64, -100.0f);
    s.db[30]            = -10.0f;

    my::dsp::CfarOptions opt;

    opt.train_cells = 0;
    CHECK(my::dsp::ca_cfar(s, opt).empty());
    CHECK(my::dsp::os_cfar(s, opt).empty());

    opt = {};
    opt.pfa = 0.0;
    CHECK(my::dsp::ca_cfar(s, opt).empty());
    CHECK(my::dsp::os_cfar(s, opt).empty());

    opt = {};
    opt.pfa = 1.0;
    CHECK(my::dsp::ca_cfar(s, opt).empty());
    CHECK(my::dsp::os_cfar(s, opt).empty());

    opt = {};
    opt.guard_cells = 1000000;
    CHECK(my::dsp::ca_cfar(s, opt).empty());
    CHECK(my::dsp::os_cfar(s, opt).empty());

    opt = {};
    opt.train_cells = 64;  // 参考窗比整条谱还长
    CHECK(my::dsp::ca_cfar(s, opt).empty());
    CHECK(my::dsp::os_cfar(s, opt).empty());

    // 门限系数对非法参数同样不产生"看起来能用"的值
    CHECK_EQ(my::dsp::cfar_ca_alpha(0, 1e-4), 0.0);
    CHECK_EQ(my::dsp::cfar_ca_alpha(8, 0.0), 0.0);
    CHECK_EQ(my::dsp::cfar_ca_alpha(8, 1.0), 0.0);
    CHECK_EQ(my::dsp::cfar_ca_alpha(8, -0.1), 0.0);
}

MYTEST_CASE(dsp_cfar_min_snr_margin) {
    // 尖峰 -80 dB，默认门限约 -89 dB（刚过 9 dB）；抬高 20 dB 余量后应当消失。
    my::dsp::Spectrum s = flat_spectrum(64, -100.0f);
    s.db[32]            = -80.0f;

    const auto base = my::dsp::ca_cfar(s);
    CHECK_EQ(base.size(), std::size_t{1});
    CHECK_EQ(base[0].bin, std::size_t{32});
    CHECK_NEAR(base[0].threshold_db, -89.05, 0.05);
    CHECK_NEAR(base[0].snr_db, 20.0, 0.05);

    my::dsp::CfarOptions opt;
    opt.min_snr_db = 5.0f;  // 门限抬到约 -84 dB，仍低于 -80 dB
    const auto with5 = my::dsp::ca_cfar(s, opt);
    CHECK_EQ(with5.size(), std::size_t{1});
    CHECK_NEAR(with5[0].threshold_db, -84.05, 0.05);

    opt.min_snr_db = 20.0f;  // 门限抬到约 -69 dB，峰被挡掉
    CHECK(my::dsp::ca_cfar(s, opt).empty());
    CHECK(my::dsp::os_cfar(s, opt).empty());
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

// 极低截止频率：float 系数的极点贴到单位圆，必须明确拒绝而不是输出发散/NaN。
MYTEST_CASE(dsp_biquad_rejects_unstable_low_frequency) {
    auto bad = my::dsp::biquad_design(my::dsp::BiquadType::Lowpass, 0.5, 48000.0, 0.707);
    CHECK(!bad);
    CHECK_EQ(bad.status().code(), my::Errc::OutOfRange);

    // 正常音频频段仍要能用
    auto ok = my::dsp::biquad_design(my::dsp::BiquadType::Lowpass, 20.0, 4000.0, 0.707);
    CHECK_RESULT(ok);
}

MYTEST_CASE(dsp_biquad_notch) {
    const double fs = 1000.0;
    Samples      x(4096);    for (std::size_t i = 0; i < x.size(); ++i) {
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

// min_len 必须按**边界细化后的真实样本数**过滤：短毛刺不能被窗口拖尾"放大"成合格突发。
MYTEST_CASE(dsp_detect_min_len_after_refine) {
    const double      fs = 1000.0;
    const std::size_t n  = 3000;
    Samples           x(n, 0.0f);

    const std::size_t short_at = 500, short_len = 8;
    const std::size_t long_at = 1500, long_len = 300;
    for (std::size_t i = 0; i < short_len; ++i) {
        x[short_at + i] = static_cast<float>(std::sin(2.0 * kPi * 100.0 * static_cast<double>(i) / fs));
    }
    for (std::size_t i = 0; i < long_len; ++i) {
        x[long_at + i] = static_cast<float>(std::sin(2.0 * kPi * 100.0 * static_cast<double>(i) / fs));
    }

    const auto bursts = my::dsp::detect_bursts(x, 32, 10.0f, /*min_len=*/64);
    CHECK_EQ(bursts.size(), std::size_t{1});
    if (!bursts.empty()) {
        CHECK(bursts[0].begin <= long_at + 64);
        CHECK(bursts[0].end >= long_at + long_len - 64);
        CHECK(bursts[0].end - bursts[0].begin >= std::size_t{64});
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

// 同一 FM 信号换采样率不应改变判决：瞬时频率门限必须按 Hz 换算，
// 直接拿 rad/sample 比的旧实现会在 fs=48k 时把干净 FM 判成 CW。
MYTEST_CASE(dsp_analyze_fm_48k_units) {
    const double fs = 48000.0;
    const double fc = 1000.0, fm = 100.0, beta = 2.0;
    Samples      x(32768);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double t = static_cast<double>(i) / fs;
        x[i] = static_cast<float>(
            std::sin(2.0 * kPi * fc * t + beta * std::sin(2.0 * kPi * fm * t)));
    }
    add_awgn(x, 40.0, 15);

    CHECK_OK_VALUE(f, my::dsp::analyze(x, fs));
    CHECK_EQ(f.sample_rate_hz, fs);
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

// 构造一个"明显 PSK"的特征集（对应 analyze() 在 BPSK 上的典型输出）：
// 恒定包络、瞬时频率有跳变（排除 CW）、但大部分时间相位不变（PSK 的判据）。
MYTEST_CASE(dsp_classify_psk) {
    my::dsp::Features f;
    f.snr_db              = 20.0;
    f.spectral_flatness   = 0.10;  // 单载波，不是噪声
    f.envelope_cv         = 0.05;  // 恒定包络（排除 AM/ASK）
    f.envelope_low_frac   = 0.02;
    f.inst_freq_std       = 0.10;  // >= 0.02，排除 CW
    f.inst_freq_quiet     = 0.75;  // 相位多数时间不变 -> PSK
    f.inst_freq_two_level = 0.20;  // 不到 FSK 的 0.60

    const auto c = my::dsp::classify(f);
    CHECK_EQ(static_cast<int>(c.modulation), static_cast<int>(my::dsp::Modulation::PSK));
    CHECK(c.confidence >= 0.8f);   // 离 0.50 的门限有 0.25 的裕量
    CHECK(c.confidence <= 1.0f);
    // classify 把输入特征原样带回来
    CHECK_NEAR(c.features.inst_freq_quiet, 0.75, 1e-12);
}

// 落在判决树缝隙里的"四不像"：包络不够平（CW 不过），又没到 0.30（AM/ASK 不过），
// 瞬时频率既没有双电平也不是持续调频 -> 兜底 Unknown，置信度显式给 0。
MYTEST_CASE(dsp_classify_unknown) {
    my::dsp::Features f;
    f.snr_db              = 10.0;
    f.spectral_flatness   = 0.20;
    f.envelope_cv         = 0.20;  // 0.15 <= cv < 0.30：CW 与 AM/ASK 之间的缝
    f.envelope_low_frac   = 0.10;
    f.inst_freq_std       = 0.01;  // < 0.02，排除 FM
    f.inst_freq_quiet     = 0.20;  // < 0.50，排除 PSK
    f.inst_freq_two_level = 0.10;  // < 0.60，排除 FSK

    const auto c = my::dsp::classify(f);
    CHECK_EQ(static_cast<int>(c.modulation), static_cast<int>(my::dsp::Modulation::Unknown));
    CHECK_NEAR(c.confidence, 0.0, 1e-12);
}

// 置信度：恒在 [0,1]，且明显不同的特征集给出明显不同的值。
// margin_confidence(v, t, s) = clamp(0.5 + 0.5*|v-t|/s, 0, 1)。
MYTEST_CASE(dsp_classify_confidence_range) {
    my::dsp::Features psk;
    psk.snr_db              = 20.0;
    psk.spectral_flatness   = 0.10;
    psk.envelope_cv         = 0.05;
    psk.inst_freq_std       = 0.10;
    psk.inst_freq_quiet     = 0.75;  // (0.75-0.50)/0.30 -> 0.9167
    psk.inst_freq_two_level = 0.20;

    my::dsp::Features fsk;  // 离 FSK 门限 0.60 只差 0.05
    fsk.snr_db              = 20.0;
    fsk.spectral_flatness   = 0.10;
    fsk.envelope_cv         = 0.05;
    fsk.inst_freq_std       = 0.10;
    fsk.inst_freq_two_level = 0.65;  // (0.65-0.60)/0.30 -> 0.5833
    fsk.inst_freq_quiet     = 0.10;

    my::dsp::Features cw;  // 落在 CW 条件正中
    cw.snr_db            = 20.0;
    cw.spectral_flatness = 0.10;
    cw.envelope_cv       = 0.0;
    cw.inst_freq_std     = 0.0;

    const auto a = my::dsp::classify(psk);
    const auto b = my::dsp::classify(fsk);
    const auto c = my::dsp::classify(cw);
    CHECK_EQ(static_cast<int>(a.modulation), static_cast<int>(my::dsp::Modulation::PSK));
    CHECK_EQ(static_cast<int>(b.modulation), static_cast<int>(my::dsp::Modulation::FSK));
    CHECK_EQ(static_cast<int>(c.modulation), static_cast<int>(my::dsp::Modulation::CW));

    for (float conf : {a.confidence, b.confidence, c.confidence}) {
        EXPECT(std::isfinite(conf));
        EXPECT(conf >= 0.0f);
        EXPECT(conf <= 1.0f);
    }
    CHECK_NEAR(a.confidence, 0.9167, 1e-3);
    CHECK_NEAR(b.confidence, 0.5833, 1e-3);
    CHECK_NEAR(c.confidence, 1.0, 1e-6);  // 完全在 CW 中心：裕量饱和
    CHECK(a.confidence > b.confidence + 0.25);  // 有区分度，不是常数
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

// 低通设计失败不能静默返回未滤波信号：截止 >= fs/2 必须报 InvalidArgument。
MYTEST_CASE(dsp_demod_rejects_invalid_audio_cutoff) {
    const double fs = 4000.0;
    Samples      x(1024);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(std::sin(2.0 * kPi * 800.0 * static_cast<double>(i) / fs));
    }

    auto am = my::dsp::demod_am(x, fs, fs);  // == Nyquist，低通无意义
    CHECK(!am);
    CHECK_EQ(am.status().code(), my::Errc::InvalidArgument);

    auto fm = my::dsp::demod_fm(x, fs, -1.0);  // 负截止
    CHECK(!fm);
    CHECK_EQ(fm.status().code(), my::Errc::InvalidArgument);
}

MYTEST_CASE(dsp_analyze_bpsk_end_to_end) {
    const double fs = 8000.0;
    const double rs = 100.0;   // 每符号 80 个采样，相位跳变只占很小一部分
    const double fc = 1000.0;
    const auto   bits = random_bits(80, 29);
    const auto   sps  = static_cast<std::size_t>(fs / rs);

    Samples x(bits.size() * sps);
    for (std::size_t k = 0; k < bits.size(); ++k) {
        const float sym = bits[k] ? 1.0f : -1.0f;
        for (std::size_t i = 0; i < sps; ++i) {
            const double n = static_cast<double>(k * sps + i);
            x[k * sps + i] = sym * static_cast<float>(std::sin(2.0 * kPi * fc * n / fs));
        }
    }
    add_awgn(x, 40.0, 30);

    CHECK_OK_VALUE(f, my::dsp::analyze(x, fs));
    CHECK(f.envelope_cv < 0.15);          // 恒定包络，排除 AM/ASK
    const auto c = my::dsp::classify(f);
    CHECK_EQ(static_cast<int>(c.modulation), static_cast<int>(my::dsp::Modulation::PSK));
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
    CHECK(ber(got, bits, /*allow_invert=*/true) < 0.02);
}

// 回归：sps 远大于样本数时，BPSK 的定时搜索会把 offset 推到信号长度之外。
// 修复前 integrate_dump_complex 里 (x.size() - offset) 为负，转 size_t 是 UB，
// 紧接着 reserve 一个巨大值会抛 length_error 穿透 Result 边界。
// 现在 offset >= x.size() 直接返回空；所有定时候选都空 -> 走"符号数不足"分支
// （实际错误码是 RuntimeError，不是 InvalidArgument）。
MYTEST_CASE(dsp_demod_bpsk_huge_sps_no_crash) {
    CSamples x(64, Complex(1.0f, 0.0f));  // 64 个样本，刚够过 "样本太少" 的检查

    const auto r = my::dsp::demod_bpsk(x, /*sample_rate=*/1000.0, /*symbol_rate=*/1.0);
    CHECK(!r.ok());
    CHECK_EQ(r.status().code(), my::Errc::RuntimeError);
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

// 回归：实测在 8192 点正弦里塞 1 个 NaN，analyze() 返回 Ok、classify() 给出
// noise/confidence=1.000，detect_bursts 返回 8065 段，welch 全 NaN。NaN/Inf 必须在
// 每个公开入口最前面就挡住，而不是让它们顺着 FFT/滤波器扩散。
// 下面的输入本来都合法（长度、采样率、符号率都够），只塞 1 个非法样本，确保命中的
// 是新增的有限性校验，而不是旧的参数检查。
MYTEST_CASE(dsp_rejects_nan_input) {
    const double      fs  = 8000.0;
    const std::size_t bad = 1234;

    Samples x = sine(100.0, fs, 4096);
    x[bad]    = std::numeric_limits<float>::quiet_NaN();

    auto invalid = [](const auto& r) {
        return !r.ok() && r.status().code() == my::Errc::InvalidArgument;
    };

    CHECK(invalid(my::dsp::welch(x, fs, 256)));
    CHECK(invalid(my::dsp::analyze(x, fs)));
    CHECK(invalid(my::dsp::demod_am(x, fs)));
    CHECK(invalid(my::dsp::demod_fm(x, fs)));
    CHECK(invalid(my::dsp::demod_ask(x, fs, 1000.0)));
    CHECK(invalid(my::dsp::demod_fsk(x, fs, 1000.0, 850.0, 750.0)));

    // 复基带入口单独测：实部/虚部任一非有限都必须拒绝
    CSamples cx(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) cx[i] = Complex(x[i], 0.0f);
    cx[bad] = Complex(std::numeric_limits<float>::quiet_NaN(), 0.0f);
    CHECK(invalid(my::dsp::demod_bpsk(cx, fs, 500.0)));

    // 消息里要带第一个非法样本下标（板端排查坏帧时全靠它）
    auto w = my::dsp::welch(x, fs, 256);
    CHECK(!w.ok());
    CHECK(w.status().message().find(std::to_string(bad)) != std::string::npos);

    // detect_bursts 是裸 vector，没有 Status 通道；文档约定非有限输入返回空表，
    // 不能让它继续产出 8065 个由 NaN 构成的假突发。
    CHECK(my::dsp::detect_bursts(x, 64).empty());
}

MYTEST_CASE(dsp_rejects_inf_input) {
    const double fs = 8000.0;

    Samples x = sine(100.0, fs, 4096);
    x[17]     = std::numeric_limits<float>::infinity();

    auto invalid = [](const auto& r) {
        return !r.ok() && r.status().code() == my::Errc::InvalidArgument;
    };

    CHECK(invalid(my::dsp::analyze(x, fs)));
    CHECK(invalid(my::dsp::welch(x, fs, 256)));

    // -Inf 同样必须被拦住（isfinite 一次检查两个方向）
    x[17] = -std::numeric_limits<float>::infinity();
    CHECK(invalid(my::dsp::analyze(x, fs)));
    CHECK(invalid(my::dsp::welch(x, fs, 256)));
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
