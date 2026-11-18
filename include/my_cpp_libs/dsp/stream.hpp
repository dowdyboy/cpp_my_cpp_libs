#pragma once
// =============================================================================
//  my::dsp 流式（分块）处理
//
//  批处理 API（dsp.hpp）一次吃一整段；板端按帧到达时，帧与帧之间的状态
//  （FIR 延迟线、IIR 状态、NCO 相位、抽取相位、AGC 增益）必须由调用方自己接。
//  这里把这些最容易接错的状态封装成对象：所有类都**跨块保持状态**，
//  块长任意（包括 0 长度、大于内部缓冲、不规则的块长）。
//
//  设计约定：
//    * 只用 Status/Result，不抛异常（可在 -fno-exceptions 下编译）；
//    * 数值上与批量实现逐点一致，**语义以批量版为准**；
//    * 0 长度块是合法输入：返回空、状态不变；
//    * 一类对象只做一类状态，不共享全局状态，可同时开多个实例。
//
//  与批量版的边界语义（最容易出分歧的地方，明确写在这里）：
//    * FirFilter：process() 输出"抽头窗口已完整"的样本，相对输入滞后
//      delay() 个样本。喂 N 个样本后共输出 max(0, N-delay()) 个；
//      流结束时调 flush()，它按零延拓补出尾部 delay() 个并复位对象。
//      process()+flush() 合起来与 fir_apply() 整段结果逐点一致
//      （批量版对整段两端都做零延拓）。
//    * Decimator：内部抗混叠 FIR 的群延迟是 8*factor 个输入样本（与批量版
//      decimate() 用的 16*factor+1 抽头 Hamming 低通完全一致）。process()
//      只输出新到达的完整符号，因此比批量结果"晚" 8 个符号；流结束时调
//      flush() 补零，输出剩余尾部。process()+flush() 的符号数和数值与
//      decimate() 整段结果完全一致（ceil(N/factor) 个）。
//    * ComplexMixer：相位累加器跨块连续，分块结果与 mix_down() 整段结果一致；
//      每块都从 0 相位重新开始会引入周期性相位跳变，这是流式混频最常见的错。
//    * BiquadFilter：每级各自保存转置直接 II 型的 z1/z2，与逐级调用
//      biquad_apply() 的批量结果一致。
//    * Agc：逐样本更新（功率估计 -> 目标增益 -> attack/release 平滑），
//      增益跨块保持。target_rms 指输出 RMS。
// =============================================================================

#include <cstddef>
#include <vector>

#include "my_cpp_libs/core/status.hpp"
#include "my_cpp_libs/dsp/dsp.hpp"
#include "my_cpp_libs/export.hpp"

namespace my::dsp {

// =============================================================================
//  状态化 FIR
// =============================================================================
/// 把样本一块一块喂进来，输出与"整段一次性 fir_apply"逐点一致。
/// process() 有 delay() 个样本的延迟（见文件头的边界语义）；flush() 补尾部。
class MYLIB_API FirFilter {
public:
    /// 前置条件：fir.taps 非空（否则 process()/flush() 返回 InvalidArgument）。
    /// 需要参数校验时用 create()。
    explicit FirFilter(Fir fir);

    /// 校验抽头非空且都是有限值。
    static Result<FirFilter> create(const Fir& fir);

    /// 处理一块。长度任意（含 0）；返回本块新完整的那部分输出。
    /// 第 1 块输出 max(0, in.size()-delay()) 个，之后每块输出 in.size() 个。
    Result<Samples> process(const Samples& in);

    /// 流结束：按零延拓补出尾部 delay() 个输出，然后把对象复位到初始状态
    /// （可接着处理下一段信号）。返回值为空时表示没有待补的尾部。
    Result<Samples> flush();

    /// 清空延迟线，回到初始状态（与构造后一致）。
    void reset() noexcept;

    std::size_t taps() const noexcept;   ///< 抽头个数
    std::size_t delay() const noexcept;  ///< 群延迟 = Fir::delay()（样本）

private:
    Fir                fir_;
    std::vector<float> history_;  ///< 最近 taps-1 个输入，初始全零
    std::size_t        total_in_ = 0;  ///< 已喂入的样本总数（不含 flush 补零）
};

// =============================================================================
//  级联双二阶（IIR）
// =============================================================================
/// 状态连续的多级 biquad。语义 = 逐级对整个信号调用 biquad_apply()。
class MYLIB_API BiquadFilter {
public:
    /// 级联至少一级；系数必须是有限值。
    static Result<BiquadFilter> create(std::vector<Biquad> stages);

    /// 返回处理后的新块（不改入参）。
    Result<Samples> process(const Samples& in);

    /// 就地处理，便于和 batch 的 biquad_apply() 写法对齐。
    void process_inplace(Samples& x);

    /// 每级状态清零，回到初始状态。
    void reset() noexcept;

    std::size_t stages() const noexcept;  ///< 级联级数

private:
    explicit BiquadFilter(std::vector<Biquad> stages);

    std::vector<Biquad> stages_;
    std::vector<float>  z1_;  ///< 每级转置直接 II 型的状态 1
    std::vector<float>  z2_;  ///< 每级转置直接 II 型的状态 2
};

// =============================================================================
//  复本振（NCO）+ 混频
// =============================================================================
/// 相位累加器跨块连续：把任意分块的样本乘上 exp(j*phi[n])，
/// phi[n] = initial_phase + n*dphi，dphi = -2*pi*freq_hz/sample_rate。
/// 与 mix_down() 在整段上逐点一致（freq_hz 相同时）。
class MYLIB_API ComplexMixer {
public:
    /// freq_hz 必须在 [0, fs/2) 内：本类把"下变频"约定为非负频率。
    /// 需要 e^{+j...} 方向时，实输入取输出共轭即可（复输入要连输入一起共轭）。
    /// 这样 create 能直接拦住负频率/负采样率这类"参数写反"的错误。
    /// sample_rate 必须为正，initial_phase 必须是有限值。
    static Result<ComplexMixer> create(double freq_hz, double sample_rate,
                                       double initial_phase = 0.0);

    /// 实输入：x[n] * exp(j*phi[n])。0 长度块返回空且不动相位。
    Result<CSamples> process(const Samples& in);

    /// 复输入：x[n] * exp(j*phi[n])。0 长度块返回空且不动相位。
    Result<CSamples> process(const CSamples& in);

    /// 相位回到 initial_phase。
    void reset() noexcept;

    /// 当前相位（弧度，未折叠，与批量 mix_down() 的累加器同口径）。
    double phase() const noexcept;

private:
    ComplexMixer(double dphi, double initial_phase) noexcept;

    double dphi_     = 0.0;
    double phase_    = 0.0;
    double initial_phase_ = 0.0;
};

// =============================================================================
//  抗混叠 FIR + 整数倍抽取
// =============================================================================
/// 与批量 decimate() 完全相同的抗混叠滤波器（16*factor+1 抽头 Hamming，
/// 截止 0.9*新奈奎斯特），跨块连续。factor == 1 时与批量版一样是直通。
class MYLIB_API Decimator {
public:
    /// factor 必须 >= 1，sample_rate 必须为正的有限值。
    static Result<Decimator> create(int factor, double sample_rate);

    /// 喂一块：只返回本块"新增的完整符号"。内部 FIR 群延迟 8*factor 个输入
    /// 样本，所以相对批量结果滞后 8 个符号左右；0 长度块返回空且不改状态。
    Result<Samples> process(const Samples& in);

    /// 流结束：按零延拓补齐尾部，返回剩余符号并把对象复位。
    /// process()+flush() 与 decimate() 整段的符号数和数值一致。
    Result<Samples> flush();

    /// 清零内部 FIR 与抽取计数器，回到初始状态。
    void reset() noexcept;

    int factor() const noexcept;  ///< 抽取倍数

private:
    Decimator(int factor, Fir anti_alias);

    /// 从 [begin, end) 中挑出 factor 的整数倍下标（且 < limit）追加到 out。
    void collect(std::size_t begin, std::size_t end, std::size_t limit,
                 const Samples& filtered, Samples& out) const;

    int         factor_ = 1;
    FirFilter   fir_;
    std::size_t input_count_    = 0;  ///< 真实输入样本数（不含 flush 补零）
    std::size_t filtered_count_ = 0;  ///< 已经过的 FIR 输出数（批量下标口径）
};

// =============================================================================
//  自动增益控制
// =============================================================================
/// AGC 参数。放在命名空间级而不是嵌套在 Agc 里，是为了让构造函数能写
/// `Options o = Options{}`：C++ 的嵌套类默认成员初始化器要等到外层类结束
/// 才解析，GCC 不接受在同一个类作用域里用它作默认实参。
struct AgcOptions {
    double target_rms = 0.2;   ///< 期望输出 RMS
    double attack     = 0.01;  ///< 增益上升时间常数（秒）
    double release    = 0.20;  ///< 增益下降时间常数（秒）
    double max_gain   = 64.0;
    double min_gain   = 1e-4;
    /// 采样率（Hz）。attack/release 以秒给出，换算成逐样本系数必须知道它；
    /// 原接口没有这个字段，这里补上，默认 48 kHz。非法值回退默认值。
    double sample_rate = 48000.0;
};

/// 逐样本 attack/release 的 RMS AGC，增益跨块保持。
/// 信号 -> 滑动均方估计 -> 目标增益（target_rms/rms，夹在 [min_gain,max_gain]）
///      -> 按"增益上升=attack / 增益下降=release"做一阶平滑 -> 乘到样本上。
class MYLIB_API Agc {
public:
    using Options = AgcOptions;  ///< 兼容 Agc::Options 写法

    /// 参数整体兜底：非有限/越界的项回退默认值，不抛异常。
    explicit Agc(Options o = Options{});

    /// 就地处理一块（长度任意，含 0）。增益跨块连续。
    void process_inplace(Samples& x);

    /// 当前增益（1.0 = 未放大/衰减）。
    double gain() const noexcept;

    /// 增益回到 1.0；功率估计回到 target_rms^2。
    void reset() noexcept;

private:
    Options opt_;
    double  gain_         = 1.0;
    double  mean_sq_      = 0.04;
    double  alpha_attack_ = 1.0;
    double  alpha_release_ = 1.0;
    double  beta_power_   = 1.0;
};

}  // namespace my::dsp
