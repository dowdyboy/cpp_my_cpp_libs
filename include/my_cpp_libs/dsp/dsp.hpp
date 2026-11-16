#pragma once
// =============================================================================
//  my::dsp —— 信号处理：频谱、滤波、检测、识别、解调
//
//  定位：把"拿到一段采样 -> 看清里面有什么 -> 认出来是什么 -> 把信息解出来"
//  这条链做成和别的组件一样的东西：Status/Result、无异常、板端可编。
//
//  底层用 pocketfft（BSD-3，单头文件，FFT 的参考实现之一，NumPy/SciPy 同源），
//  其余（滤波器、检测、特征、解调）是本库自己的实现 —— 都是"公式固定、
//  对错可测"的东西，用合成信号逐条验证过（见 tests/test_dsp.cpp）。
//
//  设计约定：
//    * 采样率一律用 Hz 显式传入，不搞全局状态；
//    * 实信号用 float，复基带用 std::complex<float>，不做 double 内部转换；
//    * **不假设有模数转换板**：API 只吃数组，接 ADC / 文件 / 网口由调用方决定；
//    * 解调是"批处理"的：一次给一段，够简单够可测。流式分块留到后面。
//
//  能力边界（v1，别指望它当 GNU Radio 用）：
//    * 已实现：频谱/PSD、FIR/IIR 滤波、混频、抽取、突发检测、
//      调制识别（规则分类器）、AM/FM/ASK/FSK/BPSK 解调；
//    * **未实现**：QAM/OFDM、信道编码、载波+定时联合恢复的最优实现、
//      多信号分离、盲均衡。需要这些时再引 liquid-dsp（见 docs/08）。
// =============================================================================

#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "my_cpp_libs/core/status.hpp"
#include "my_cpp_libs/export.hpp"

namespace my::dsp {

using Complex = std::complex<float>;
using Samples = std::vector<float>;
using CSamples = std::vector<Complex>;

// =============================================================================
//  窗口 / 变换
// =============================================================================
enum class Window { Rect, Hann, Hamming, Blackman };

/// 生成窗函数（归一化到峰值 1.0）
MYLIB_API std::vector<float> make_window(Window type, std::size_t n);

/// 复数 FFT。nfft = 0 时取输入长度；大于输入长度时补零，小于则截断。
MYLIB_API Result<CSamples> fft(const CSamples& x, std::size_t nfft = 0);

/// 逆变换（不除以 N，保持 FFT/ifft 对称；调用方自己按需缩放）
MYLIB_API Result<CSamples> ifft(const CSamples& x);

// =============================================================================
//  频谱
// =============================================================================
struct Spectrum {
    double             sample_rate = 0.0;
    std::size_t        nfft        = 0;
    std::vector<float> freq_hz;  ///< 单边谱频率轴，0 ~ fs/2
    std::vector<float> db;       ///< dBFS：满幅正弦的峰 = 0 dB

    /// 噪声底（dB）：取全谱中位数，对窄带信号不敏感
    float noise_floor_db = -120.0f;
    /// 最强谱线的位置
    std::size_t peak_bin  = 0;
    double      peak_freq = 0.0;
    float       peak_db   = -120.0f;

    std::size_t size() const noexcept { return db.size(); }
};

/// Welch 平均周期图。overlap ∈ [0,1)，nfft 建议取 2 的幂（pocketfft 会处理任意长度，
/// 但非 2 幂明显慢）。
MYLIB_API Result<Spectrum> welch(const Samples& x, double sample_rate, std::size_t nfft,
                                 float overlap = 0.5f, Window window = Window::Hann);

/// 在频谱上找峰：局部极大 + 高于噪声底 min_prominence_db + 与已选峰间隔 >= min_sep_hz。
struct Peak {
    std::size_t bin  = 0;
    double      freq = 0.0;
    float       db   = 0.0f;
};
MYLIB_API std::vector<Peak> find_peaks(const Spectrum& s, float min_prominence_db = 6.0f,
                                       double min_sep_hz = 0.0, std::size_t max_peaks = 8);

/// 信噪比粗估（dB）：最强峰功率 - 噪声底。信号本身弱于噪声底时返回 0。
MYLIB_API float estimate_snr_db(const Spectrum& s);

// =============================================================================
//  滤波器
// =============================================================================
/// 对称 FIR（线性相位），taps 个数必须是奇数，中心抽头在中间
struct Fir {
    std::vector<float> taps;
    std::size_t        order() const noexcept { return taps.empty() ? 0 : taps.size() - 1; }
    std::size_t        delay() const noexcept { return order() / 2; }  ///< 群延迟（样本）
};

MYLIB_API Result<Fir> fir_lowpass(double cutoff_hz, double sample_rate, std::size_t num_taps,
                                  Window window = Window::Hamming);
MYLIB_API Result<Fir> fir_highpass(double cutoff_hz, double sample_rate, std::size_t num_taps,
                                   Window window = Window::Hamming);
MYLIB_API Result<Fir> fir_bandpass(double low_hz, double high_hz, double sample_rate,
                                   std::size_t num_taps, Window window = Window::Hamming);

/// 卷积并按群延迟对齐（输出与输入等长、时间轴不偏）
MYLIB_API Result<Samples> fir_apply(const Fir& fir, const Samples& x);

/// RBJ 双二阶（IIR）。q 越大越窄；Notch 用 q 控制陷波宽度。
enum class BiquadType { Lowpass, Highpass, Bandpass, Notch };
struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
};
MYLIB_API Result<Biquad> biquad_design(BiquadType type, double freq_hz, double sample_rate,
                                       double q = 0.70710678);
/// 就地把 IIR 应用到整个信号（内部用转置直接 II 型状态）
MYLIB_API void biquad_apply(const Biquad& bq, Samples& x);

// =============================================================================
//  混频 / 抽取
// =============================================================================
/// 下变频：x[n] * exp(-j2π f n / fs)，把 f 处的信号搬到 0 Hz
MYLIB_API Result<CSamples> mix_down(const Samples& x, double freq_hz, double sample_rate);

/// 复信号混频（推荐）：对已经下变频的复基带再搬一次，或对解析信号混频。
/// 注意实信号版本会留下 -2f 的镜像，需要"干净搬移"时先取解析信号再走这个重载。
MYLIB_API Result<CSamples> mix_down(const CSamples& x, double freq_hz, double sample_rate);

/// 整数倍抽取（先低通防混叠）。factor = 1 时原样返回。
MYLIB_API Result<Samples> decimate(const Samples& x, int factor, double sample_rate);

// =============================================================================
//  检测
// =============================================================================
struct Burst {
    std::size_t begin = 0;  ///< 起始样本
    std::size_t end   = 0;  ///< 结束样本（不含）
    float       peak_db = 0.0f;
};

/// 突发检测：滑窗 RMS -> 相对噪声底的 dB -> 门限 -> 膨胀合并 -> 最短长度过滤。
/// @param window      滑窗长度（样本），建议 >= 1 个符号周期
/// @param threshold_db 高出噪声底多少 dB 算"有信号"
/// @param min_len     短于它的段丢弃（样本数）；0 表示按 window 的一半
MYLIB_API std::vector<Burst> detect_bursts(const Samples& x, std::size_t window,
                                           float threshold_db = 10.0f, std::size_t min_len = 0);

// =============================================================================
//  特征 / 识别
// =============================================================================
/// 单段信号的调制特征。全部是"可解释、可单独验证"的量，不做黑盒。
struct Features {
    double center_freq_hz = 0.0;  ///< 频谱重心（相对 0 Hz 的偏移）
    double bandwidth_hz   = 0.0;  ///< -3 dB 带宽（只对单载波有意义）
    double snr_db         = 0.0;

    double envelope_cv       = 0.0;  ///< 包络变异系数 std/mean：AM/ASK 大，CW/FM/PSK 小
    double envelope_low_frac = 0.0;  ///< 包络低于 25% 峰值的样本比例：ASK 高，AM 低
    double inst_freq_std     = 0.0;  ///< 瞬时频率标准差（rad/sample，已去均值）
    double inst_freq_two_level = 0.0;///< 瞬时频率落在两个主电平附近的比例：FSK 高，FM 低
    double inst_freq_quiet   = 0.0;  ///< 瞬时频率接近 0 的样本比例：PSK 高（多数时间相位不变），FM 低
    double spectral_flatness = 0.0;  ///< 谱平坦度：噪声接近 1，单音接近 0
    double spectral_symmetry = 0.0;  ///< 谱关于峰对称性 [0,1]
};

MYLIB_API Result<Features> analyze(const Samples& x, double sample_rate);

enum class Modulation { Unknown, Noise, CW, AM, ASK, FM, FSK, PSK };
MYLIB_API const char* modulation_name(Modulation m) noexcept;

struct Classification {
    Modulation modulation = Modulation::Unknown;
    float      confidence = 0.0f;  ///< [0,1]，规则边界的裕量，不是概率
    Features   features;
};

/// 规则分类器：按文档里的判决树走，输入是 Features。
/// **不是机器学习模型**：干净的单信号准；多信号/强多径/低 SNR 时会给出 Unknown，
/// 这比"猜一个错的"更有用。
MYLIB_API Classification classify(const Features& f);

// =============================================================================
//  解调
// =============================================================================
/// AM：包络检波 + 去直流 + 低通（截止默认 0.5 × 符号率，未知时用 fs/200）
MYLIB_API Result<Samples> demod_am(const Samples& x, double sample_rate,
                                   double audio_cutoff_hz = 0.0);

/// FM：正交鉴频（瞬时频率）+ 去直流 + 低通
MYLIB_API Result<Samples> demod_fm(const Samples& x, double sample_rate,
                                   double audio_cutoff_hz = 0.0);

/// ASK/OOK：包络 -> 符号周期积分 -> 门限判决。threshold < 0 时用 (max+min)/2。
MYLIB_API Result<std::vector<std::uint8_t>> demod_ask(const Samples& x, double sample_rate,
                                                      double symbol_rate,
                                                      float  threshold = -1.0f);

/// 2-FSK：鉴频 -> 符号周期积分 -> 判 mark（1）/ space（0）
MYLIB_API Result<std::vector<std::uint8_t>> demod_fsk(const Samples& x, double sample_rate,
                                                      double symbol_rate, double mark_hz,
                                                      double space_hz);

/// BPSK：平方环估载波频偏 -> 去旋转 -> 符号定时搜索 -> 判决。
/// 输入是**复基带**（已下变频到 0 附近，允许有频偏，但不能还是射频采样）。
/// 存在 π 的相位模糊（BPSK 的固有性质），需要前导码/差分编码来定相。
MYLIB_API Result<std::vector<std::uint8_t>> demod_bpsk(const CSamples& x, double sample_rate,
                                                       double symbol_rate);

/// 把 [0,1) 软符号按 0.5 判决成比特（测试/后处理常用）
MYLIB_API std::vector<std::uint8_t> slice_bits(const Samples& soft);

}  // namespace my::dsp
