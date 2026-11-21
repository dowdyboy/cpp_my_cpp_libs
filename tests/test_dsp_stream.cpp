// =============================================================================
//  my::dsp 流式处理单元测试
//
//  重点不是"能跑"，而是"分块结果与批量结果逐点一致"：
//    * FIR：process 有 delay() 的延迟，flush() 补尾部后与 fir_apply 完全一致；
//    * Biquad：级联状态跨块连续，与逐级 biquad_apply 一致；
//    * Mixer：相位累加器跨块连续（每块从 0 开始会出周期性相位跳变）；
//    * Decimator：process 只给完整符号，flush 后与 decimate 的长度/数值一致；
//    * AGC：增益跨块保持，收敛到 target_rms 且受 max_gain 限制。
//
//  固定随机种子，不依赖时钟；每个用例都在毫秒级。
// =============================================================================

#include "my_cpp_libs/dsp/dsp.hpp"
#include "my_cpp_libs/dsp/stream.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "test_framework.hpp"

/// CHECK_OK_VALUE 绑定的是 const 引用；流式对象要在后续调用里改状态，
/// 这里给一个可写版本（只在 void 测试函数里用）。
#define CHECK_OK_MUTABLE(name, expr)                                          \
    auto name##_result = (expr);                                              \
    if (!name##_result) {                                                     \
        std::printf("         实际错误: %s\n",                               \
                    name##_result.status().to_string().c_str());              \
        ::mytest::fail(#expr " 应当返回结果", __FILE__, __LINE__);            \
        return;                                                               \
    }                                                                         \
    auto& name = name##_result.value()

MYTEST_SUITE("dsp_stream");

namespace {

constexpr double kPi = 3.14159265358979323846;

using my::dsp::Biquad;
using my::dsp::Complex;
using my::dsp::CSamples;
using my::dsp::Samples;

Samples random_signal(std::size_t n, std::uint32_t seed) {
    std::mt19937                          rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    Samples                               x(n);
    for (auto& v : x) v = dist(rng);
    return x;
}

Samples sine(double freq_hz, double fs, std::size_t n, double amp = 1.0, double phase = 0.0) {
    Samples x(n);
    for (std::size_t i = 0; i < n; ++i) {
        x[i] = static_cast<float>(
            amp * std::sin(2.0 * kPi * freq_hz * static_cast<double>(i) / fs + phase));
    }
    return x;
}

Samples add(Samples a, const Samples& b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) a[i] += b[i];
    return a;
}

Samples scaled(Samples x, float k) {
    for (auto& v : x) v *= k;
    return x;
}

template <class T>
void append_all(std::vector<T>& dst, const std::vector<T>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

/// 用给定块长模式（循环使用）把 x 喂给任意流式对象，返回拼接后的输出。
/// 返回 false 表示 process() 失败；调用方用 CHECK 报错。
template <class Stream, class Input, class Output>
bool feed_blocks(Stream& stream, const Input& x, const std::vector<std::size_t>& pattern,
                 Output& out) {
    if (pattern.empty()) return false;

    std::size_t pos = 0;
    std::size_t bi  = 0;
    while (pos < x.size()) {
        std::size_t want = pattern[bi % pattern.size()];
        ++bi;
        if (want > x.size() - pos) want = x.size() - pos;
        const Input block(x.begin() + static_cast<std::ptrdiff_t>(pos),
                          x.begin() + static_cast<std::ptrdiff_t>(pos + want));
        auto r = stream.process(block);
        if (!r) return false;
        append_all(out, r.value());
        pos += want;
    }
    return true;
}

/// [begin, end) 区间的 RMS
double rms_from(const Samples& x, std::size_t begin) {
    if (begin >= x.size()) return 0.0;
    double sum = 0.0;
    for (std::size_t i = begin; i < x.size(); ++i) {
        sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    }
    return std::sqrt(sum / static_cast<double>(x.size() - begin));
}

}  // namespace

// -----------------------------------------------------------------------------
//  1. FIR：不规则分块 + flush 后与 fir_apply 逐点一致
// -----------------------------------------------------------------------------
MYTEST_CASE(dsp_stream_fir_matches_batch) {
    const double      fs = 1000.0;
    const std::size_t n  = 777;
    Samples           x  = add(random_signal(n, 12345u), sine(80.0, fs, n, 0.5));

    CHECK_OK_VALUE(lp, my::dsp::fir_lowpass(120.0, fs, 31, my::dsp::Window::Hamming));
    CHECK_OK_VALUE(batch, my::dsp::fir_apply(lp, x));

    CHECK_OK_MUTABLE(filt, my::dsp::FirFilter::create(lp));
    CHECK_EQ(filt.taps(), lp.taps.size());
    CHECK_EQ(filt.delay(), lp.delay());

    const std::vector<std::size_t> pattern{1, 7, 13, 100, 3, 64, 2, 31};
    Samples                        streamed;
    CHECK(feed_blocks(filt, x, pattern, streamed));

    // process() 只给"窗口已完整"的部分：比整段少 delay() 个
    CHECK_EQ(streamed.size(), n - filt.delay());

    const std::size_t d = filt.delay();
    CHECK_OK_VALUE(tail, filt.flush());
    CHECK_EQ(tail.size(), d);
    append_all(streamed, tail);

    CHECK_EQ(streamed.size(), batch.size());
    for (std::size_t i = 0; i < batch.size(); ++i) {
        CHECK_NEAR(streamed[i], batch[i], 1e-6);
    }
}

// -----------------------------------------------------------------------------
//  2. FIR：两种分块方式结果一致（块长不变性）
// -----------------------------------------------------------------------------
MYTEST_CASE(dsp_stream_fir_block_size_invariance) {
    const double      fs = 1000.0;
    const std::size_t n  = 501;
    Samples           x  = add(random_signal(n, 999u), sine(200.0, fs, n, 0.3));

    CHECK_OK_VALUE(lp, my::dsp::fir_lowpass(250.0, fs, 21, my::dsp::Window::Hamming));
    CHECK_OK_MUTABLE(f_regular, my::dsp::FirFilter::create(lp));
    CHECK_OK_MUTABLE(f_irregular, my::dsp::FirFilter::create(lp));

    Samples a;
    CHECK(feed_blocks(f_regular, x, {64}, a));
    CHECK_OK_VALUE(tail_a, f_regular.flush());
    append_all(a, tail_a);

    Samples b;
    CHECK(feed_blocks(f_irregular, x, {1, 2, 3, 5, 8, 13, 40, 97}, b));
    CHECK_OK_VALUE(tail_b, f_irregular.flush());
    append_all(b, tail_b);

    CHECK_EQ(a.size(), b.size());
    CHECK_EQ(a.size(), n);
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK_NEAR(a[i], b[i], 1e-7);
    }
}

// -----------------------------------------------------------------------------
//  3. Biquad：2~3 级级联与批量 biquad_apply 一致
// -----------------------------------------------------------------------------
MYTEST_CASE(dsp_stream_biquad_matches_batch) {
    const double      fs = 1000.0;
    const std::size_t n  = 2000;
    Samples x = add(add(sine(50.0, fs, n, 1.0), sine(200.0, fs, n, 0.7)),
                    scaled(random_signal(n, 2024u), 0.1f));

    CHECK_OK_VALUE(lowpass,
                   my::dsp::biquad_design(my::dsp::BiquadType::Lowpass, 300.0, fs, 0.70710678));
    CHECK_OK_VALUE(notch,
                   my::dsp::biquad_design(my::dsp::BiquadType::Notch, 50.0, fs, 5.0));
    CHECK_OK_VALUE(highpass,
                   my::dsp::biquad_design(my::dsp::BiquadType::Highpass, 20.0, fs, 0.70710678));

    const std::vector<Biquad> stages{lowpass, notch, highpass};
    Samples                   batch = x;
    for (const auto& bq : stages) my::dsp::biquad_apply(bq, batch);

    CHECK_OK_MUTABLE(filt, my::dsp::BiquadFilter::create(stages));
    CHECK_EQ(filt.stages(), stages.size());

    Samples streamed;
    CHECK(feed_blocks(filt, x, {17, 1, 300, 9, 64}, streamed));
    CHECK_EQ(streamed.size(), batch.size());
    for (std::size_t i = 0; i < batch.size(); ++i) {
        CHECK_NEAR(streamed[i], batch[i], 1e-5);
    }

    // process_inplace 与 process 必须走同一套状态/公式
    CHECK_OK_MUTABLE(filt2, my::dsp::BiquadFilter::create(stages));
    Samples inplace = x;
    filt2.process_inplace(inplace);
    for (std::size_t i = 0; i < batch.size(); ++i) {
        CHECK_NEAR(inplace[i], batch[i], 1e-5);
    }
}

// -----------------------------------------------------------------------------
//  4. Mixer：相位累加器跨块连续（分两块 == 整块）
// -----------------------------------------------------------------------------
MYTEST_CASE(dsp_stream_mixer_phase_continuity) {
    const double      fs = 1000.0;
    const double      f0 = 37.5;
    const std::size_t n  = 1001;
    const Samples     x  = add(random_signal(n, 4242u), sine(50.0, fs, n, 0.8));

    CHECK_OK_VALUE(batch, my::dsp::mix_down(x, f0, fs));
    CHECK_OK_MUTABLE(mix, my::dsp::ComplexMixer::create(f0, fs));

    CSamples streamed;
    CHECK(feed_blocks(mix, x, {333, 1, 100, 7, 500}, streamed));
    CHECK_EQ(streamed.size(), batch.size());
    for (std::size_t i = 0; i < batch.size(); ++i) {
        CHECK_NEAR(streamed[i].real(), batch[i].real(), 1e-6);
        CHECK_NEAR(streamed[i].imag(), batch[i].imag(), 1e-6);
    }

    // 相位必须等于 n * dphi（而不是每块重置后只累计最后一块）
    const double expect_phase = -2.0 * kPi * f0 * static_cast<double>(n) / fs;
    CHECK_NEAR(mix.phase(), expect_phase, 1e-9);

    // 复输入重载：同样跨块连续
    CSamples xc(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = 2.0 * kPi * 50.0 * static_cast<double>(i) / fs;
        xc[i] = Complex(static_cast<float>(0.8 * std::cos(ph)),
                        static_cast<float>(0.8 * std::sin(ph)));
    }
    CHECK_OK_VALUE(batch_c, my::dsp::mix_down(xc, f0, fs));
    CHECK_OK_MUTABLE(mix_c, my::dsp::ComplexMixer::create(f0, fs));
    CSamples streamed_c;
    CHECK(feed_blocks(mix_c, xc, {500, 501}, streamed_c));
    CHECK_EQ(streamed_c.size(), batch_c.size());
    for (std::size_t i = 0; i < batch_c.size(); ++i) {
        CHECK_NEAR(streamed_c[i].real(), batch_c[i].real(), 1e-6);
        CHECK_NEAR(streamed_c[i].imag(), batch_c[i].imag(), 1e-6);
    }
}

// -----------------------------------------------------------------------------
//  5. Decimator：分块 + flush 后与 decimate 的长度/数值一致
//
//  边界语义（以批量版为准）：
//    批量 decimate = 对整段做零延拓 FIR（16*factor+1 抽头，群延迟 8*factor）
//    -> 取下标 0, factor, 2*factor, ...（< N）。
//    流式 process() 只输出"抗混叠 FIR 窗口已完整"的符号，因此相对批量结果
//    滞后约 8 个符号；flush() 按零延拓补齐尾部并复位，合起来与批量完全一致。
// -----------------------------------------------------------------------------
MYTEST_CASE(dsp_stream_decimator_matches_batch) {
    const double      fs     = 1000.0;
    const int         factor = 4;
    const std::size_t n      = 1501;  // 故意不是 factor 的整数倍
    Samples           x = add(add(sine(50.0, fs, n, 1.0), sine(400.0, fs, n, 0.5)),
                              scaled(random_signal(n, 777u), 0.05f));

    CHECK_OK_VALUE(batch, my::dsp::decimate(x, factor, fs));
    CHECK_EQ(batch.size(), (n + static_cast<std::size_t>(factor) - 1) /
                               static_cast<std::size_t>(factor));
    CHECK_EQ(batch.size(), 376u);

    CHECK_OK_MUTABLE(dec, my::dsp::Decimator::create(factor, fs));
    CHECK_EQ(dec.factor(), factor);

    Samples streamed;
    CHECK(feed_blocks(dec, x, {100, 1, 277, 33, 7, 250}, streamed));

    // 未 flush 时只输出了 8*factor 个输入样本延迟之后的完整符号
    const std::size_t group_delay    = 8 * static_cast<std::size_t>(factor);
    const std::size_t complete_input = n - group_delay;
    const std::size_t expect_prefix =
        (complete_input + static_cast<std::size_t>(factor) - 1) /
        static_cast<std::size_t>(factor);
    CHECK_EQ(streamed.size(), expect_prefix);

    CHECK_OK_VALUE(tail, dec.flush());
    CHECK_EQ(tail.size(), batch.size() - streamed.size());
    append_all(streamed, tail);

    CHECK_EQ(streamed.size(), batch.size());
    for (std::size_t i = 0; i < batch.size(); ++i) {
        CHECK_NEAR(streamed[i], batch[i], 1e-5);
    }

    // factor == 1 与批量版一致：直通，无滤波、无延迟
    CHECK_OK_MUTABLE(dec1, my::dsp::Decimator::create(1, fs));
    Samples passthrough;
    CHECK(feed_blocks(dec1, x, {13, 200, 1}, passthrough));
    CHECK_EQ(passthrough.size(), x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        CHECK_NEAR(passthrough[i], x[i], 1e-7);
    }
    CHECK_OK_VALUE(tail1, dec1.flush());
    CHECK(tail1.empty());
}

// -----------------------------------------------------------------------------
//  6. reset() 回到初始状态：同一段信号两次输出完全相同
// -----------------------------------------------------------------------------
MYTEST_CASE(dsp_stream_reset_restores_state) {
    const double      fs = 1000.0;
    const std::size_t n  = 400;
    const Samples     x  = add(random_signal(n, 31337u), sine(100.0, fs, n, 0.6));

    // --- FIR ---
    CHECK_OK_VALUE(lp, my::dsp::fir_lowpass(150.0, fs, 15, my::dsp::Window::Hamming));
    CHECK_OK_MUTABLE(fir, my::dsp::FirFilter::create(lp));

    Samples first;
    CHECK(feed_blocks(fir, x, {37, 1, 100}, first));
    CHECK_OK_VALUE(tail1, fir.flush());
    append_all(first, tail1);

    fir.reset();
    CHECK_EQ(fir.taps(), 15u);
    Samples second;
    CHECK(feed_blocks(fir, x, {37, 1, 100}, second));
    CHECK_OK_VALUE(tail2, fir.flush());
    append_all(second, tail2);

    CHECK_EQ(first.size(), second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        CHECK_NEAR(first[i], second[i], 0.0);  // 状态完全复现 -> 逐位相同
    }

    // --- Biquad ---
    CHECK_OK_VALUE(bq, my::dsp::biquad_design(my::dsp::BiquadType::Lowpass, 150.0, fs));
    std::vector<Biquad> stages{bq};
    CHECK_OK_MUTABLE(filt, my::dsp::BiquadFilter::create(stages));

    CHECK_OK_VALUE(y1, filt.process(x));
    filt.reset();
    CHECK_OK_VALUE(y2, filt.process(x));
    CHECK_EQ(y1.size(), y2.size());
    for (std::size_t i = 0; i < y1.size(); ++i) {
        CHECK_NEAR(y1[i], y2[i], 0.0);
    }

    // --- Mixer ---
    CHECK_OK_MUTABLE(mix, my::dsp::ComplexMixer::create(42.0, fs));
    CHECK_OK_VALUE(c1, mix.process(x));
    CHECK(mix.phase() != 0.0);
    mix.reset();
    CHECK_NEAR(mix.phase(), 0.0, 0.0);
    CHECK_OK_VALUE(c2, mix.process(x));
    CHECK_EQ(c1.size(), c2.size());
    for (std::size_t i = 0; i < c1.size(); ++i) {
        CHECK_NEAR(c1[i].real(), c2[i].real(), 0.0);
        CHECK_NEAR(c1[i].imag(), c2[i].imag(), 0.0);
    }

    // --- AGC ---
    my::dsp::Agc::Options ao;
    ao.sample_rate = fs;
    my::dsp::Agc agc(ao);
    Samples     amplified = x;
    agc.process_inplace(amplified);
    CHECK(agc.gain() != 1.0);
    agc.reset();
    CHECK_NEAR(agc.gain(), 1.0, 0.0);
}

// -----------------------------------------------------------------------------
//  7. AGC：恒定幅度正弦 -> 输出 RMS 收敛到 target_rms，增益方向正确
// -----------------------------------------------------------------------------
MYTEST_CASE(dsp_stream_agc_converges) {
    const double      fs = 1000.0;
    const std::size_t n  = 8000;
    const Samples     x  = sine(100.0, fs, n, 1.0);

    my::dsp::Agc::Options opt;
    opt.target_rms = 0.2;
    opt.attack     = 0.01;
    opt.release    = 0.20;
    opt.max_gain   = 64.0;
    opt.sample_rate = fs;

    my::dsp::Agc agc(opt);
    const double g0 = agc.gain();
    CHECK_NEAR(g0, 1.0, 0.0);

    Samples            y = x;
    std::vector<double> gains;
    std::size_t         pos = 0;
    while (pos < y.size()) {
        const std::size_t len = std::min<std::size_t>(100, y.size() - pos);
        Samples block(y.begin() + static_cast<std::ptrdiff_t>(pos),
                      y.begin() + static_cast<std::ptrdiff_t>(pos + len));
        agc.process_inplace(block);
        std::copy(block.begin(), block.end(), y.begin() + static_cast<std::ptrdiff_t>(pos));
        gains.push_back(agc.gain());
        pos += len;
    }

    // 强信号：增益必须整体下降（gains[0]=100 样本后，gains[4]=500，gains[9]=1000）
    CHECK(gains.size() >= 10);
    CHECK(gains[0] < g0);
    CHECK(gains[4] < gains[0]);
    CHECK(gains[9] < gains[4]);
    CHECK(agc.gain() < gains[9]);

    // 收敛到 target_rms（正弦幅度 1 -> 增益约 0.2/0.707）：给 ±30% 宽容差
    const double expected_gain = opt.target_rms / std::sqrt(0.5);
    CHECK_NEAR(agc.gain(), expected_gain, 0.30 * expected_gain);

    // 输出 RMS（跳过前 1/4 的收敛过程）
    const double out_rms = rms_from(y, y.size() / 4);
    CHECK_NEAR(out_rms, opt.target_rms, 0.30 * opt.target_rms);
    CHECK(agc.gain() >= opt.min_gain);
    CHECK(agc.gain() <= opt.max_gain);

    agc.reset();
    CHECK_NEAR(agc.gain(), 1.0, 0.0);
}

// 一个 NaN/Inf 样本不能把 AGC 功率估计永久污染（旧实现会回退 max_gain，
// 整路放大 64 倍且永不恢复）。
MYTEST_CASE(dsp_stream_agc_survives_nonfinite_sample) {
    const double      fs = 1000.0;
    const std::size_t n  = 6000;
    Samples           x  = sine(100.0, fs, n, 0.5);

    my::dsp::Agc::Options opt;
    opt.target_rms  = 0.2;
    opt.sample_rate = fs;
    my::dsp::Agc agc(opt);

    Samples block(x.begin(), x.begin() + 10);
    agc.process_inplace(block);
    block.assign(1, std::numeric_limits<float>::quiet_NaN());
    agc.process_inplace(block);
    CHECK(std::isfinite(block[0]));

    Samples rest(x.begin() + 11, x.end());
    agc.process_inplace(rest);

    CHECK(std::isfinite(agc.gain()));
    CHECK(agc.gain() < 2.0);            // 干净流应约 0.566，绝不能爬到 64
    CHECK(agc.gain() >= opt.min_gain);

    const double out_rms = rms_from(rest, rest.size() / 2);
    CHECK(std::isfinite(out_rms));
    CHECK(out_rms < 1.0);
}

// -----------------------------------------------------------------------------
//  8. AGC：极弱信号时增益不超过 max_gain
// -----------------------------------------------------------------------------
MYTEST_CASE(dsp_stream_agc_respects_limits) {
    const double      fs = 1000.0;
    const std::size_t n  = 4000;
    const double      amp = 1e-3;
    const Samples     x  = sine(100.0, fs, n, amp);

    my::dsp::Agc::Options opt;
    opt.target_rms  = 0.2;
    opt.attack      = 0.01;
    opt.release     = 0.20;
    opt.max_gain    = 64.0;
    opt.min_gain    = 1e-4;
    opt.sample_rate = fs;

    my::dsp::Agc agc(opt);
    Samples     y       = x;
    double      max_gain_seen = agc.gain();

    std::size_t pos = 0;
    while (pos < y.size()) {
        const std::size_t len = std::min<std::size_t>(64, y.size() - pos);
        Samples block(y.begin() + static_cast<std::ptrdiff_t>(pos),
                      y.begin() + static_cast<std::ptrdiff_t>(pos + len));
        agc.process_inplace(block);
        std::copy(block.begin(), block.end(), y.begin() + static_cast<std::ptrdiff_t>(pos));
        max_gain_seen = std::max(max_gain_seen, agc.gain());
        pos += len;
    }

    // 需要约 0.2 / (1e-3/sqrt(2)) = 283 倍，但被 max_gain 夹住
    CHECK(max_gain_seen <= opt.max_gain + 1e-9);
    CHECK(agc.gain() <= opt.max_gain + 1e-9);
    CHECK(agc.gain() > 0.90 * opt.max_gain);  // 确实顶到上限附近
    CHECK(agc.gain() >= opt.min_gain);

    // 输出电压不能超过 amp * max_gain
    const double limit = amp * opt.max_gain + 1e-9;
    for (float v : y) {
        CHECK(std::fabs(static_cast<double>(v)) <= limit);
    }

    // 全零输入：增益可以升到 max_gain，但绝不允许越界/非有限
    my::dsp::Agc quiet(opt);
    Samples      zeros(500, 0.0f);
    quiet.process_inplace(zeros);
    CHECK(quiet.gain() <= opt.max_gain + 1e-9);
    CHECK(std::isfinite(quiet.gain()));
}

// -----------------------------------------------------------------------------
//  9. 0 长度块与长度 1 的块
// -----------------------------------------------------------------------------
MYTEST_CASE(dsp_stream_zero_and_short_blocks) {
    const double      fs = 1000.0;
    const std::size_t n  = 100;
    const Samples     x  = add(random_signal(n, 2025u), sine(60.0, fs, n, 0.5));

    CHECK_OK_VALUE(bq, my::dsp::biquad_design(my::dsp::BiquadType::Lowpass, 120.0, fs));
    std::vector<Biquad> stages{bq};

    // --- 长度 1 的块连续喂 100 次 == 一次喂 100 个 ---
    CHECK_OK_MUTABLE(f_single, my::dsp::BiquadFilter::create(stages));
    Samples one_by_one;
    for (std::size_t i = 0; i < x.size(); ++i) {
        Samples one(1, x[i]);
        CHECK_OK_VALUE(y, f_single.process(one));
        append_all(one_by_one, y);
    }
    CHECK_OK_MUTABLE(f_all, my::dsp::BiquadFilter::create(stages));
    CHECK_OK_VALUE(all_at_once, f_all.process(x));

    CHECK_EQ(one_by_one.size(), all_at_once.size());
    for (std::size_t i = 0; i < all_at_once.size(); ++i) {
        CHECK_NEAR(one_by_one[i], all_at_once[i], 1e-9);
    }

    // --- 0 长度块返回空且不改变 Biquad 状态 ---
    CHECK_OK_MUTABLE(f_zero, my::dsp::BiquadFilter::create(stages));
    Samples half1, half2;
    CHECK(feed_blocks(f_zero, Samples(x.begin(), x.begin() + 40), {40}, half1));
    CHECK_OK_VALUE(empty_out, f_zero.process(Samples{}));
    CHECK(empty_out.empty());
    CHECK(feed_blocks(f_zero, Samples(x.begin() + 40, x.end()), {60}, half2));
    append_all(half1, half2);
    CHECK_EQ(half1.size(), all_at_once.size());
    for (std::size_t i = 0; i < half1.size(); ++i) {
        CHECK_NEAR(half1[i], all_at_once[i], 1e-9);
    }

    // --- 0 长度块不改变 FIR 状态 ---
    CHECK_OK_VALUE(lp, my::dsp::fir_lowpass(150.0, fs, 9, my::dsp::Window::Hamming));
    CHECK_OK_MUTABLE(fir, my::dsp::FirFilter::create(lp));
    Samples fir_out;
    CHECK(feed_blocks(fir, Samples(x.begin(), x.begin() + 37), {37}, fir_out));
    CHECK_OK_VALUE(fir_empty, fir.process(Samples{}));
    CHECK(fir_empty.empty());
    CHECK(feed_blocks(fir, Samples(x.begin() + 37, x.end()), {63}, fir_out));
    CHECK_OK_VALUE(fir_tail, fir.flush());
    append_all(fir_out, fir_tail);
    CHECK_OK_VALUE(fir_batch, my::dsp::fir_apply(lp, x));
    CHECK_EQ(fir_out.size(), fir_batch.size());
    for (std::size_t i = 0; i < fir_batch.size(); ++i) {
        CHECK_NEAR(fir_out[i], fir_batch[i], 1e-6);
    }

    // --- 0 长度块不改变 Decimator 状态 ---
    CHECK_OK_MUTABLE(dec_zero, my::dsp::Decimator::create(4, fs));
    Samples dec_out;
    CHECK(feed_blocks(dec_zero, Samples(x.begin(), x.begin() + 37), {37}, dec_out));
    CHECK_OK_VALUE(dec_empty, dec_zero.process(Samples{}));
    CHECK(dec_empty.empty());
    CHECK(feed_blocks(dec_zero, Samples(x.begin() + 37, x.end()), {63}, dec_out));
    CHECK_OK_VALUE(dec_tail, dec_zero.flush());
    append_all(dec_out, dec_tail);
    CHECK_OK_VALUE(dec_batch, my::dsp::decimate(x, 4, fs));
    CHECK_EQ(dec_out.size(), dec_batch.size());
    for (std::size_t i = 0; i < dec_batch.size(); ++i) {
        CHECK_NEAR(dec_out[i], dec_batch[i], 1e-5);
    }

    // --- 0 长度块不改变 Mixer 相位 ---
    CHECK_OK_MUTABLE(mix, my::dsp::ComplexMixer::create(25.0, fs));
    CHECK_OK_VALUE(m1, mix.process(x));
    const double phase_before = mix.phase();
    CHECK_OK_VALUE(mix_empty_r, mix.process(Samples{}));
    CHECK(mix_empty_r.empty());
    CHECK_OK_VALUE(mix_empty_c, mix.process(CSamples{}));
    CHECK(mix_empty_c.empty());
    CHECK_NEAR(mix.phase(), phase_before, 0.0);
    CHECK_OK_VALUE(m2, mix.process(x));
    CHECK_EQ(m1.size(), m2.size());

    // --- 0 长度块不改变 AGC 增益 ---
    my::dsp::Agc::Options ao;
    ao.sample_rate = fs;
    my::dsp::Agc agc(ao);
    Samples     agc_in = x;
    agc.process_inplace(agc_in);
    const double g_before = agc.gain();
    Samples      empty;
    agc.process_inplace(empty);
    CHECK_NEAR(agc.gain(), g_before, 0.0);
}

// -----------------------------------------------------------------------------
//  10. 非法参数
// -----------------------------------------------------------------------------
MYTEST_CASE(dsp_stream_invalid_args) {
    // Decimator：factor 必须 >= 1，采样率必须为正
    CHECK_ERR(my::dsp::Decimator::create(0, 1000.0));
    CHECK_ERR(my::dsp::Decimator::create(-4, 1000.0));
    CHECK_ERR(my::dsp::Decimator::create(4, 0.0));
    CHECK_ERR(my::dsp::Decimator::create(4, -1000.0));

    // ComplexMixer：负频率 / 负采样率 / 超奈奎斯特 / 非有限值
    CHECK_ERR(my::dsp::ComplexMixer::create(-100.0, 1000.0));
    CHECK_ERR(my::dsp::ComplexMixer::create(100.0, -1000.0));
    CHECK_ERR(my::dsp::ComplexMixer::create(100.0, 0.0));
    CHECK_ERR(my::dsp::ComplexMixer::create(600.0, 1000.0));
    CHECK_ERR(my::dsp::ComplexMixer::create(100.0, 1000.0,
                                            std::numeric_limits<double>::quiet_NaN()));

    // FirFilter：空抽头 / 非有限抽头
    my::dsp::Fir empty_fir;
    CHECK_ERR(my::dsp::FirFilter::create(empty_fir));

    my::dsp::Fir nan_fir;
    nan_fir.taps = {1.0f, std::numeric_limits<float>::quiet_NaN(), 1.0f};
    CHECK_ERR(my::dsp::FirFilter::create(nan_fir));

    // BiquadFilter：空级联 / 非有限系数
    CHECK_ERR(my::dsp::BiquadFilter::create(std::vector<Biquad>{}));

    my::dsp::Biquad bad;
    bad.b0 = std::numeric_limits<float>::infinity();
    CHECK_ERR(my::dsp::BiquadFilter::create(std::vector<Biquad>{bad}));
}
