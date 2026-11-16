#include "my_cpp_libs/dsp/dsp.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "pocketfft_hdronly.h"

#include "common.hpp"

namespace my::dsp {
namespace {

using detail::kTwoPi;

Result<CSamples> transform(const CSamples& x, std::size_t nfft, bool forward) {
    if (x.empty() && nfft == 0) {
        return Status(Errc::InvalidArgument, "FFT 输入为空");
    }
    const std::size_t n = (nfft != 0) ? nfft : x.size();
    if (n == 0) return Status(Errc::InvalidArgument, "FFT 长度不能为 0");

    CSamples in(n, Complex(0.0f, 0.0f));
    std::copy_n(x.begin(), std::min(x.size(), n), in.begin());
    CSamples out(n);

    pocketfft::shape_t  shape{n};
    pocketfft::stride_t stride{static_cast<std::ptrdiff_t>(sizeof(Complex))};
    pocketfft::c2c(shape, stride, stride, {0},
                   forward ? pocketfft::FORWARD : pocketfft::BACKWARD,
                   in.data(), out.data(), 1.0f, 1);
    return out;
}

}  // namespace

std::vector<float> make_window(Window type, std::size_t n) {
    std::vector<float> w(n, 1.0f);
    if (n < 2 || type == Window::Rect) return w;

    const double denom = static_cast<double>(n - 1);
    for (std::size_t i = 0; i < n; ++i) {
        const double x = kTwoPi * static_cast<double>(i) / denom;
        switch (type) {
            case Window::Hann:
                w[i] = static_cast<float>(0.5 - 0.5 * std::cos(x));
                break;
            case Window::Hamming:
                w[i] = static_cast<float>(0.54 - 0.46 * std::cos(x));
                break;
            case Window::Blackman:
                w[i] = static_cast<float>(0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2.0 * x));
                break;
            case Window::Rect:
            default:
                break;
        }
    }
    return w;
}

Result<CSamples> fft(const CSamples& x, std::size_t nfft) {
    return transform(x, nfft, /*forward=*/true);
}

Result<CSamples> ifft(const CSamples& x) {
    return transform(x, 0, /*forward=*/false);
}

// -----------------------------------------------------------------------------
//  Welch PSD
// -----------------------------------------------------------------------------
Result<Spectrum> welch(const Samples& x, double sample_rate, std::size_t nfft, float overlap,
                       Window window) {
    if (x.empty()) return Status(Errc::InvalidArgument, "输入信号为空");
    if (sample_rate <= 0.0) return Status(Errc::InvalidArgument, "采样率必须为正");
    if (nfft < 16) return Status(Errc::InvalidArgument, "nfft 至少 16");
    if (x.size() < nfft) {
        return Status(Errc::InvalidArgument,
                      "信号长度 " + std::to_string(x.size()) + " 小于 nfft " + std::to_string(nfft));
    }
    if (overlap < 0.0f || overlap >= 1.0f) {
        return Status(Errc::InvalidArgument, "overlap 必须在 [0,1) 内");
    }

    const std::vector<float> win = make_window(window, nfft);
    const double             wsum = std::accumulate(win.begin(), win.end(), 0.0);
    if (wsum <= 0.0) return Status(Errc::RuntimeError, "窗函数和为零");

    std::size_t hop = static_cast<std::size_t>(
        std::lround(static_cast<double>(nfft) * (1.0 - static_cast<double>(overlap))));
    hop = std::max<std::size_t>(1, hop);

    const std::size_t           bins = nfft / 2 + 1;
    std::vector<double>         acc(bins, 0.0);
    std::size_t                 segments = 0;
    std::vector<std::complex<float>> buf(nfft);

    for (std::size_t start = 0; start + nfft <= x.size(); start += hop) {
        // 去均值：直流不参与 PSD（否则噪声底被直流泄漏抬高）
        double mean = 0.0;
        for (std::size_t i = 0; i < nfft; ++i) mean += x[start + i];
        mean /= static_cast<double>(nfft);

        for (std::size_t i = 0; i < nfft; ++i) {
            buf[i] = Complex(static_cast<float>((x[start + i] - mean) * win[i]), 0.0f);
        }
        auto spec = fft(buf, nfft);
        if (!spec) return spec.status();

        for (std::size_t k = 0; k < bins; ++k) {
            const double re = spec.value()[k].real();
            const double im = spec.value()[k].imag();
            acc[k] += re * re + im * im;
        }
        ++segments;
    }

    if (segments == 0) return Status(Errc::RuntimeError, "没有完整的分析窗");

    Spectrum s;
    s.sample_rate = sample_rate;
    s.nfft        = nfft;
    s.freq_hz.resize(bins);
    s.db.resize(bins);

    // 幅度归一化：满幅（幅度 1.0）正弦在峰值 bin 上 = 0 dB。
    // 窗后 DFT 在峰值 bin 上 |X| = A * Wsum / 2（Wsum = Σw），
    // 所以幅度估计 A = 2|X| / Wsum。（注意这里**没有** N：DFT 的求和里
    // 已经含了 N 个样本，再乘 N 会把满幅正弦算成 -60 dB。）
    const double denom = wsum;
    for (std::size_t k = 0; k < bins; ++k) {
        const double mean_power = acc[k] / static_cast<double>(segments);
        const double amp_scale  = (k == 0 || k == bins - 1) ? 1.0 : 2.0;
        const double amp        = amp_scale * std::sqrt(mean_power) / denom;
        s.freq_hz[k] = static_cast<float>(static_cast<double>(k) * sample_rate /
                                         static_cast<double>(nfft));
        s.db[k]      = detail::amp_to_db(static_cast<float>(amp));
    }

    std::vector<float> sorted_db = s.db;
    s.noise_floor_db = detail::median(std::move(sorted_db));

    const auto it = std::max_element(s.db.begin(), s.db.end());
    s.peak_bin  = static_cast<std::size_t>(it - s.db.begin());
    s.peak_db   = *it;
    s.peak_freq = s.freq_hz[s.peak_bin];
    return s;
}

std::vector<Peak> find_peaks(const Spectrum& s, float min_prominence_db, double min_sep_hz,
                             std::size_t max_peaks) {
    std::vector<Peak> out;
    if (s.db.size() < 3 || max_peaks == 0) return out;

    const float floor_db = s.noise_floor_db + min_prominence_db;

    struct Cand {
        std::size_t bin;
        float       db;
    };
    std::vector<Cand> cands;
    for (std::size_t k = 1; k + 1 < s.db.size(); ++k) {
        if (s.db[k] >= floor_db && s.db[k] > s.db[k - 1] && s.db[k] >= s.db[k + 1]) {
            cands.push_back({k, s.db[k]});
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.db > b.db; });

    for (const auto& c : cands) {
        if (out.size() >= max_peaks) break;
        bool too_close = false;
        for (const auto& p : out) {
            if (std::fabs(static_cast<double>(s.freq_hz[c.bin]) - p.freq) < min_sep_hz) {
                too_close = true;
                break;
            }
        }
        if (!too_close) out.push_back(Peak{c.bin, s.freq_hz[c.bin], c.db});
    }

    std::sort(out.begin(), out.end(), [](const Peak& a, const Peak& b) { return a.freq < b.freq; });
    return out;
}

float estimate_snr_db(const Spectrum& s) {
    const float snr = s.peak_db - s.noise_floor_db;
    return snr > 0.0f ? snr : 0.0f;
}

namespace detail {

std::size_t next_pow2(std::size_t n) {
    std::size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

float median(std::vector<float> v) {
    if (v.empty()) return 0.0f;
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    return v[mid];
}

Result<CSamples> analytic(const Samples& x) {
    const std::size_t n = x.size();
    if (n < 8) return Status(Errc::InvalidArgument, "解析信号至少要 8 个样本");

    const std::size_t nfft = next_pow2(n);
    CSamples          in(nfft, Complex(0.0f, 0.0f));
    for (std::size_t i = 0; i < n; ++i) in[i] = Complex(x[i], 0.0f);

    auto spec = fft(in, nfft);
    if (!spec) return spec.status();

    // 解析信号：DC 与 Nyquist 保持，正频率 ×2，负频率置零
    CSamples& X = spec.value();
    for (std::size_t k = 1; k < nfft / 2; ++k) X[k] *= 2.0f;
    for (std::size_t k = nfft / 2 + 1; k < nfft; ++k) X[k] = Complex(0.0f, 0.0f);

    auto t = ifft(X);
    if (!t) return t.status();

    const float scale = 1.0f / static_cast<float>(nfft);
    CSamples    out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = t.value()[i] * scale;
    return out;
}

void unwrap_phase(Samples& phase) {
    if (phase.size() < 2) return;
    // 注意：必须用**原始**相位做相邻差，不能拿"已修正的值"去比 ——
    // 否则一旦修正过一次，后续每一步都会再差一个 2π，越修越离谱。
    float offset   = 0.0f;
    float previous = phase[0];
    for (std::size_t i = 1; i < phase.size(); ++i) {
        const float raw = phase[i];
        const float d   = raw - previous;
        if (d > static_cast<float>(kPi)) {
            offset -= static_cast<float>(kTwoPi);
        } else if (d < -static_cast<float>(kPi)) {
            offset += static_cast<float>(kTwoPi);
        }
        phase[i] = raw + offset;
        previous = raw;
    }
}

Result<Samples> instantaneous_frequency(const CSamples& a, bool remove_mean) {
    if (a.size() < 3) return Status(Errc::InvalidArgument, "样本太少");
    Samples phase(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) phase[i] = std::arg(a[i]);
    unwrap_phase(phase);

    Samples df(phase.size() - 1);
    double  mean = 0.0;
    for (std::size_t i = 1; i < phase.size(); ++i) {
        df[i - 1] = phase[i] - phase[i - 1];
        mean += df[i - 1];
    }
    if (remove_mean) {
        mean /= static_cast<double>(df.size());
        for (auto& v : df) v -= static_cast<float>(mean);
    }
    return df;
}

double two_level_ratio(const float* v, std::size_t n, int bins) {
    if (v == nullptr || n == 0 || bins < 2) return 0.0;
    float lo = v[0], hi = v[0];
    for (std::size_t i = 1; i < n; ++i) {
        lo = std::min(lo, v[i]);
        hi = std::max(hi, v[i]);
    }
    if (!(hi > lo)) return 1.0;  // 常数信号：只在"一个电平"上，按"集中"处理

    std::vector<int> hist(static_cast<std::size_t>(bins), 0);
    const double     scale = static_cast<double>(bins - 1) / static_cast<double>(hi - lo);
    for (std::size_t i = 0; i < n; ++i) {
        int b = static_cast<int>(std::lround((v[i] - lo) * scale));
        b     = std::clamp(b, 0, bins - 1);
        ++hist[static_cast<std::size_t>(b)];
    }

    int first = -1, second = -1;
    for (int b = 0; b < bins; ++b) {
        if (first < 0 || hist[static_cast<std::size_t>(b)] > hist[static_cast<std::size_t>(first)]) {
            second = first;
            first  = b;
        } else if (second < 0 ||
                   hist[static_cast<std::size_t>(b)] > hist[static_cast<std::size_t>(second)]) {
            second = b;
        }
    }
    if (second < 0) return 1.0;

    // 两个主峰各自 ±1 个 bin 都算"落在电平上"
    int covered = 0;
    for (int b = 0; b < bins; ++b) {
        if (std::abs(b - first) <= 1 || std::abs(b - second) <= 1) {
            covered += hist[static_cast<std::size_t>(b)];
        }
    }
    return static_cast<double>(covered) / static_cast<double>(n);
}

Result<Samples> integrate_dump(const Samples& x, double samples_per_symbol, double offset) {
    if (samples_per_symbol <= 1.0) {
        return Status(Errc::InvalidArgument, "每符号采样数必须 > 1");
    }
    if (offset < 0.0 || offset >= samples_per_symbol) {
        return Status(Errc::InvalidArgument, "offset 必须落在 [0, sps) 内");
    }
    // offset 是 double，可能大于 x.size()：先判断再相减，避免负数转 size_t（UB）
    if (offset >= static_cast<double>(x.size())) {
        return Status(Errc::InvalidArgument, "样本数不足一个符号");
    }
    const std::size_t nsym = static_cast<std::size_t>(
        (static_cast<double>(x.size()) - offset) / samples_per_symbol);
    if (nsym == 0) return Status(Errc::InvalidArgument, "样本数不足一个符号");

    Samples out(nsym, 0.0f);
    for (std::size_t k = 0; k < nsym; ++k) {
        const std::size_t begin = static_cast<std::size_t>(
            std::llround(offset + static_cast<double>(k) * samples_per_symbol));
        const std::size_t end = std::min(
            x.size(), static_cast<std::size_t>(
                          std::llround(offset + static_cast<double>(k + 1) * samples_per_symbol)));
        if (end <= begin) continue;
        double sum = 0.0;
        for (std::size_t i = begin; i < end; ++i) sum += x[i];
        out[k] = static_cast<float>(sum / static_cast<double>(end - begin));
    }
    return out;
}

}  // namespace detail
}  // namespace my::dsp
