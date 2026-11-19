#include "my_cpp_libs/dsp/dsp.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "common.hpp"

namespace my::dsp {
namespace {

using detail::kPi;

struct Stats {
    double mean = 0.0;
    double std  = 0.0;
    double max  = 0.0;
    double min  = 0.0;
};

/// 去掉两端各 10% 再统计：解析信号在两端有边缘效应，
/// 不裁会把"干净的单音"也算出 10% 的包络起伏。
Stats trimmed_stats(const std::vector<float>& v) {
    Stats s;
    if (v.empty()) return s;
    const std::size_t margin = v.size() / 10;
    const std::size_t begin  = margin;
    const std::size_t end    = v.size() - margin;
    if (end <= begin) return s;

    double sum = 0.0;
    s.max      = v[begin];
    s.min      = v[begin];
    for (std::size_t i = begin; i < end; ++i) {
        sum += v[i];
        s.max = std::max(s.max, static_cast<double>(v[i]));
        s.min = std::min(s.min, static_cast<double>(v[i]));
    }
    s.mean = sum / static_cast<double>(end - begin);

    double var = 0.0;
    for (std::size_t i = begin; i < end; ++i) {
        const double d = static_cast<double>(v[i]) - s.mean;
        var += d * d;
    }
    s.std = std::sqrt(var / static_cast<double>(end - begin));
    return s;
}

}  // namespace

// -----------------------------------------------------------------------------
//  突发检测
// -----------------------------------------------------------------------------
std::vector<Burst> detect_bursts(const Samples& x, std::size_t window, float threshold_db,
                                 std::size_t min_len) {
    std::vector<Burst> out;
    if (x.empty() || window == 0 || x.size() < window) return out;

    // 前缀和 -> 滑窗 RMS，O(N)
    std::vector<double> prefix(x.size() + 1, 0.0);
    for (std::size_t i = 0; i < x.size(); ++i) {
        prefix[i + 1] = prefix[i] + static_cast<double>(x[i]) * x[i];
    }
    const std::size_t nwin = x.size() - window + 1;
    std::vector<float> rms(nwin);
    for (std::size_t j = 0; j < nwin; ++j) {
        const double p = (prefix[j + window] - prefix[j]) / static_cast<double>(window);
        rms[j]         = static_cast<float>(std::sqrt(p));
    }

    // 噪声底：RMS 的 10% 分位数。
    // 用中位数在多突发、占空比高的采集里会失效（中位数落在信号电平上，
    // 门限被抬到信号之上，一个突发都检不出来）。10% 分位数的假设是
    // "至少有 10% 的时间是纯噪声"，这对突发通信是合理的；连续信号不适合用突发检测。
    // 全零区间会让它变成 0，这里夹一个极小值，语义变成"任何非零片段都算信号"。
    std::vector<float> sorted_rms = rms;
    std::nth_element(sorted_rms.begin(),
                     sorted_rms.begin() + static_cast<std::ptrdiff_t>(sorted_rms.size() / 10),
                     sorted_rms.end());
    float ref = sorted_rms[sorted_rms.size() / 10];
    if (ref < 1e-9f) ref = 1e-9f;
    const float thr = ref * std::pow(10.0f, threshold_db / 20.0f);

    if (min_len == 0) min_len = std::max<std::size_t>(1, window / 2);

    std::size_t i = 0;
    while (i < nwin) {
        if (rms[i] < thr) {
            ++i;
            continue;
        }
        std::size_t j    = i;
        float       peak = rms[i];
        while (j + 1 < nwin && rms[j + 1] >= thr) {
            ++j;
            peak = std::max(peak, rms[j]);
        }
        const std::size_t begin = i;
        const std::size_t end   = j + window;
        if (end - begin >= min_len) {
            Burst b;
            b.begin   = begin;
            b.end     = end;
            b.peak_db = detail::amp_to_db(peak / ref);
            out.push_back(b);
        }
        i = j + 1;
    }

    // ---- 边界细化 ----
    // 粗边界是按窗口算的，最多会早/晚一个窗口（上面 window 个样本）。
    // 这里用 16 点滑窗幅度往回收，收到"超过突发峰值 10%"的位置为止。
    // 不细化的话，下游拿到的段会带一截静音，ASK/FSK 的符号定时会被带偏。
    if (!out.empty()) {
        constexpr int     kHalf   = 8;
        std::vector<float> prefix_abs(x.size() + 1, 0.0f);
        for (std::size_t k = 0; k < x.size(); ++k) {
            prefix_abs[k + 1] = prefix_abs[k] + std::fabs(x[k]);
        }
        auto env_at = [&](std::size_t idx) {
            const std::size_t lo = (idx > static_cast<std::size_t>(kHalf)) ? idx - kHalf : 0;
            const std::size_t hi = std::min(x.size(), idx + static_cast<std::size_t>(kHalf));
            if (hi <= lo) return 0.0f;
            return (prefix_abs[hi] - prefix_abs[lo]) / static_cast<float>(hi - lo);
        };

        for (auto& b : out) {
            float peak_env = 0.0f;
            for (std::size_t k = b.begin; k < b.end; ++k) peak_env = std::max(peak_env, env_at(k));
            const float edge = 0.1f * peak_env;
            std::size_t lo = b.begin;
            std::size_t hi = b.end;
            while (lo + 1 < hi && env_at(lo) < edge) ++lo;
            while (hi > lo + 1 && env_at(hi - 1) < edge) --hi;
            b.begin = lo;
            b.end   = hi;
        }
    }

    return out;
}

// -----------------------------------------------------------------------------
//  CFAR 检测
// -----------------------------------------------------------------------------
double cfar_ca_alpha(std::size_t train_cells, double pfa) {
    if (train_cells == 0 || !(pfa > 0.0 && pfa < 1.0)) return 0.0;
    const double n = 2.0 * static_cast<double>(train_cells);
    return n * (std::pow(pfa, -1.0 / n) - 1.0);
}

namespace {

/// dBFS -> 线性功率。Spectrum::db 的 0 dB 是"满幅正弦"，功率域用 10^(db/10)
/// 就得到相对满幅的功率；CFAR 只用相对量，归一化常数会同时出现在门限里。
double db_to_power(float db) {
    return std::pow(10.0, static_cast<double>(db) / 10.0);
}

/// CA / OS 共用的扫描骨架。ordered=false 取参考窗的平均功率，true 取第 rank 小。
///
/// 两端各 half = guard_cells + train_cells 个 bin 没有完整参考窗，直接不参与检测
/// （docs/08 §11 写明了这条约定）。参考窗最多 2*train_cells 个 float，
/// 整帧复用同一块，不在循环里分配。
std::vector<Detection> cfar_scan(const Spectrum& s, const CfarOptions& opt, bool ordered) {
    std::vector<Detection> out;
    const std::size_t      n = s.db.size();
    if (n == 0 || n != s.freq_hz.size()) return out;
    if (opt.train_cells == 0) return out;
    if (!(opt.pfa > 0.0 && opt.pfa < 1.0)) return out;
    // 先保证 train <= n，后面的 n - train 才不会回绕。
    if (opt.train_cells > n) return out;
    if (opt.guard_cells > n - opt.train_cells) return out;

    const std::size_t half = opt.guard_cells + opt.train_cells;  // <= n，不会溢出
    if (half > (n - 1) / 2) return out;  // 连一个完整参考窗都放不下

    const std::size_t nref  = 2 * opt.train_cells;
    const double      alpha = cfar_ca_alpha(opt.train_cells, opt.pfa);
    if (!(alpha > 0.0)) return out;

    // OS 的 rank 是 1-based（0 = 自动 ceil(3N/4)）。超范围时夹到 [1, N]：
    // 一个可夹的 rank 不值得把整帧判成非法。
    std::size_t rank = opt.os_rank;
    if (rank == 0) rank = nref - nref / 4;  // ceil(3N/4)，避免 3*N 溢出
    if (rank > nref) rank = nref;

    // min_snr_db 是功率 dB，线性域余量 = 10^(x/10)；<= 0 时不额外抬高。
    const double margin = (opt.min_snr_db > 0.0f)
                              ? std::pow(10.0, static_cast<double>(opt.min_snr_db) / 10.0)
                              : 1.0;

    std::vector<float> ref(nref);
    bool               in_run = false;
    Detection          best;

    for (std::size_t k = half; k + half < n; ++k) {
        // 参考窗：左侧 [k-half, k-guard)，右侧 (k+guard, k+half]，保护单元留在中间。
        std::size_t m = 0;
        for (std::size_t i = k - half; i < k - opt.guard_cells; ++i) {
            ref[m++] = static_cast<float>(db_to_power(s.db[i]));
        }
        for (std::size_t i = k + opt.guard_cells + 1; i <= k + half; ++i) {
            ref[m++] = static_cast<float>(db_to_power(s.db[i]));
        }

        double noise_power = 0.0;
        if (ordered) {
            const std::size_t idx = rank - 1;  // 第 rank 小 -> 0-based 下标
            std::nth_element(ref.begin(),
                             ref.begin() + static_cast<std::ptrdiff_t>(idx), ref.end());
            noise_power = static_cast<double>(ref[idx]);
        } else {
            double sum = 0.0;
            for (float p : ref) sum += static_cast<double>(p);
            noise_power = sum / static_cast<double>(nref);
        }

        const double threshold = alpha * noise_power * margin;
        const float  thr_db    = detail::power_to_db(static_cast<float>(threshold));
        if (s.db[k] > thr_db) {
            // 连续超门限的 bin 归成一个检测，保留其中最强（db 最大）的那个。
            if (!in_run || s.db[k] > best.db) {
                const float snr_db =
                    s.db[k] - detail::power_to_db(static_cast<float>(noise_power));
                best = Detection{k, s.freq_hz[k], s.db[k], thr_db, snr_db};
            }
            in_run = true;
        } else if (in_run) {
            out.push_back(best);
            in_run = false;
        }
    }
    if (in_run) out.push_back(best);
    return out;
}

}  // namespace

std::vector<Detection> ca_cfar(const Spectrum& s, const CfarOptions& opt) {
    return cfar_scan(s, opt, /*ordered=*/false);
}

std::vector<Detection> os_cfar(const Spectrum& s, const CfarOptions& opt) {
    return cfar_scan(s, opt, /*ordered=*/true);
}

// -----------------------------------------------------------------------------
//  特征
// -----------------------------------------------------------------------------
Result<Features> analyze(const Samples& x, double sample_rate) {
    if (x.size() < 64) return Status(Errc::InvalidArgument, "至少需要 64 个样本");
    if (sample_rate <= 0.0) return Status(Errc::InvalidArgument, "采样率必须为正");

    std::size_t nfft = detail::next_pow2(x.size() / 4);
    nfft             = std::clamp<std::size_t>(nfft, 64, 2048);
    if (nfft > x.size()) nfft = detail::next_pow2(x.size());
    if (nfft > x.size()) return Status(Errc::InvalidArgument, "信号太短");

    auto spec = welch(x, sample_rate, nfft, 0.5f, Window::Hann);
    if (!spec) return spec.status();
    const Spectrum& s = spec.value();

    Features f;
    f.snr_db     = estimate_snr_db(s);
    f.center_freq_hz = 0.0;
    f.bandwidth_hz   = 0.0;

    // 线性功率 + 总功率
    std::vector<double> power(s.db.size());
    double              total = 0.0;
    for (std::size_t k = 0; k < s.db.size(); ++k) {
        power[k] = std::pow(10.0, static_cast<double>(s.db[k]) / 10.0);
        total += power[k];
    }

    if (total > 0.0) {
        double centroid = 0.0;
        for (std::size_t k = 0; k < s.db.size(); ++k) centroid += s.freq_hz[k] * power[k];
        f.center_freq_hz = centroid / total;
    }

    // -3 dB 带宽：从峰往两边走
    {
        const float target = s.peak_db - 3.0f;
        std::size_t lo     = s.peak_bin;
        std::size_t hi     = s.peak_bin;
        while (lo > 0 && s.db[lo] > target) --lo;
        while (hi + 1 < s.db.size() && s.db[hi] > target) ++hi;
        f.bandwidth_hz = static_cast<double>(s.freq_hz[hi] - s.freq_hz[lo]);
    }

    // 谱平坦度：几何平均 / 算术平均
    {
        double log_sum = 0.0;
        double sum     = 0.0;
        std::size_t n  = 0;
        for (std::size_t k = 1; k + 1 < power.size(); ++k) {
            const double p = std::max(power[k], 1e-20);
            log_sum += std::log(p);
            sum += p;
            ++n;
        }
        if (n > 0 && sum > 0.0) {
            f.spectral_flatness = std::exp(log_sum / static_cast<double>(n)) /
                                  (sum / static_cast<double>(n));
        }
    }

    // 关于峰的对称性：min/max 比
    {
        const std::size_t pk   = s.peak_bin;
        const std::size_t span = std::min(pk, s.db.size() - 1 - pk);
        double            num = 0.0, den = 0.0;
        for (std::size_t k = 1; k <= span; ++k) {
            const double a = std::max(power[pk - k], 1e-20);
            const double b = std::max(power[pk + k], 1e-20);
            num += std::min(a, b);
            den += std::max(a, b);
        }
        f.spectral_symmetry = (den > 0.0) ? num / den : 0.0;
    }

    // 解析信号：包络 + 瞬时频率
    auto analytic = detail::analytic(x);
    if (!analytic) return analytic.status();
    const CSamples& a = analytic.value();

    std::vector<float> envelope(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) envelope[i] = std::abs(a[i]);

    const Stats env = trimmed_stats(envelope);
    if (env.mean > 1e-9) f.envelope_cv = env.std / env.mean;

    // 低于 25% 峰值的比例（用裁剪后的区间）
    {
        const std::size_t margin = envelope.size() / 10;
        const std::size_t begin  = margin;
        const std::size_t end    = envelope.size() - margin;
        const double      thr    = 0.25 * env.max;
        if (end > begin && thr > 0.0) {
            std::size_t low = 0;
            for (std::size_t i = begin; i < end; ++i) {
                if (envelope[i] < thr) ++low;
            }
            f.envelope_low_frac = static_cast<double>(low) / static_cast<double>(end - begin);
        }
    }

    auto df = detail::instantaneous_frequency(a);
    if (!df) return df.status();
    {
        // 瞬时频率对噪声非常敏感（相位求导会放大白噪声），
        // 先做 5 点滑动平均再统计，否则 40 dB SNR 的纯 CW 都会被算成"有频率调制"。
        Samples v = df.value();
        if (v.size() > 16) {
            Samples sm(v.size());
            constexpr int kHalf = 2;
            for (std::size_t i = 0; i < v.size(); ++i) {
                const std::size_t lo = (i >= static_cast<std::size_t>(kHalf)) ? i - kHalf : 0;
                const std::size_t hi =
                    std::min(v.size() - 1, i + static_cast<std::size_t>(kHalf));
                double acc = 0.0;
                for (std::size_t j = lo; j <= hi; ++j) acc += v[j];
                sm[i] = static_cast<float>(acc / static_cast<double>(hi - lo + 1));
            }
            v.swap(sm);
        }

        const std::size_t margin = v.size() / 10;
        const std::size_t begin  = margin;
        const std::size_t end    = v.size() - margin;
        if (end > begin + 1) {
            double mean = 0.0, var = 0.0;
            for (std::size_t i = begin; i < end; ++i) mean += v[i];
            mean /= static_cast<double>(end - begin);
            for (std::size_t i = begin; i < end; ++i) {
                const double d = static_cast<double>(v[i]) - mean;
                var += d * d;
            }
            f.inst_freq_std = std::sqrt(var / static_cast<double>(end - begin));

            // "接近 0" 的判据用 std 而不是 max：瞬时频率的重尾让 max 被噪声尖峰
            // 支配，PSK（多数时间相位不变 + 少数跳变）才能和 FM（连续调制）分开。
            const float quiet_thr = static_cast<float>(0.25 * f.inst_freq_std);
            std::vector<float> dev(end - begin);
            std::size_t        quiet = 0;
            for (std::size_t i = begin; i < end; ++i) {
                const float d = static_cast<float>(static_cast<double>(v[i]) - mean);
                dev[i - begin] = d;
                if (std::fabs(d) < quiet_thr) ++quiet;
            }
            f.inst_freq_quiet = static_cast<double>(quiet) / static_cast<double>(dev.size());
            f.inst_freq_two_level = detail::two_level_ratio(dev.data(), dev.size());
        }
    }

    return f;
}

// -----------------------------------------------------------------------------
//  识别
// -----------------------------------------------------------------------------
const char* modulation_name(Modulation m) noexcept {
    switch (m) {
        case Modulation::Noise: return "noise";
        case Modulation::CW: return "CW";
        case Modulation::AM: return "AM";
        case Modulation::ASK: return "ASK";
        case Modulation::FM: return "FM";
        case Modulation::FSK: return "FSK";
        case Modulation::PSK: return "PSK";
        case Modulation::Unknown:
        default: return "unknown";
    }
}

namespace {

float margin_confidence(double value, double threshold, double scale) {
    const double m = std::fabs(value - threshold) / std::max(scale, 1e-9);
    return static_cast<float>(std::clamp(0.5 + 0.5 * m, 0.0, 1.0));
}

}  // namespace

Classification classify(const Features& f) {
    Classification c;
    c.features = f;

    // 判决树（阈值都是在合成信号上标定的，见 tests/test_dsp.cpp）。
    // 顺序有意义：噪声 -> 单音 -> 调幅 -> 调频/FSK -> PSK -> 兜底。
    if (f.snr_db < 6.0 || f.spectral_flatness > 0.55) {
        c.modulation = Modulation::Noise;
        c.confidence = std::clamp(static_cast<float>((6.0 - f.snr_db) / 6.0 + 0.4), 0.0f, 1.0f);
        return c;
    }

    if (f.envelope_cv < 0.15 && f.inst_freq_std < 0.02) {
        c.modulation = Modulation::CW;
        c.confidence = margin_confidence(f.inst_freq_std, 0.02, 0.02);
        return c;
    }

    if (f.envelope_cv >= 0.30) {
        if (f.envelope_low_frac >= 0.30) {
            c.modulation = Modulation::ASK;
            c.confidence = margin_confidence(f.envelope_low_frac, 0.30, 0.30);
        } else {
            c.modulation = Modulation::AM;
            c.confidence = margin_confidence(f.envelope_low_frac, 0.30, 0.30);
        }
        return c;
    }

    if (f.inst_freq_two_level >= 0.60) {
        c.modulation = Modulation::FSK;
        c.confidence = margin_confidence(f.inst_freq_two_level, 0.60, 0.30);
        return c;
    }

    if (f.inst_freq_quiet >= 0.50) {
        c.modulation = Modulation::PSK;
        c.confidence = margin_confidence(f.inst_freq_quiet, 0.50, 0.30);
        return c;
    }

    if (f.inst_freq_std >= 0.02) {
        c.modulation = Modulation::FM;
        c.confidence = margin_confidence(f.inst_freq_std, 0.02, 0.05);
        return c;
    }

    c.modulation = Modulation::Unknown;
    c.confidence = 0.0f;
    return c;
}

}  // namespace my::dsp
