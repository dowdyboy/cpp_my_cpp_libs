// =============================================================================
//  example_signal_pipeline —— 信号处理全链路演示
//
//  一段"抓到的采样"里放进 4 个不同调制的突发信号，然后：
//    1. 频谱：Welch PSD + 找峰 + 噪声底/SNR；
//    2. 检测：滑窗 RMS 找突发；
//    3. 识别：对每个突发算特征、判调制；
//    4. 解调：把 FSK 那个突发解成比特，和生成时用的比特对一下 BER。
//
//  不接任何硬件：信号是程序里合成的。真实场景里把 x 换成 ADC/文件/网口来的
//  float 数组即可，后面的代码一行不用改。
// =============================================================================

#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "my_cpp_libs/dsp/dsp.hpp"
#include "my_cpp_libs/log/format.hpp"
#include "my_cpp_libs/log/log.hpp"

namespace {

constexpr double kPi  = 3.14159265358979323846;
constexpr double kFs  = 8000.0;  // 采样率

using my::dsp::Samples;

/// 往 dst 末尾追加 n 个零（模拟突发之间的间隔）
void append_silence(Samples& dst, std::size_t n) { dst.insert(dst.end(), n, 0.0f); }

/// 在 dst 末尾追加一段正弦
void append_tone(Samples& dst, double freq, std::size_t n, double amp = 0.5) {
    for (std::size_t i = 0; i < n; ++i) {
        dst.push_back(static_cast<float>(amp * std::sin(2.0 * kPi * freq *
                                                        static_cast<double>(i) / kFs)));
    }
}

}  // namespace

int main() {
    my::log::Config cfg;
    cfg.level      = my::log::Level::Info;
    cfg.to_console = true;
    my::log::init(cfg);

    // -------------------------------------------------------------------------
    // 1. 合成一段 3 秒的"采集"：CW / AM / FM / FSK 四个突发，各自频率不同
    // -------------------------------------------------------------------------
    Samples            rx;
    std::vector<float> marks;  // 记录每个突发的真实中心频率（只用于打印对照）

    // ① CW @ 600 Hz，0.25 s
    marks.push_back(600.0f);
    append_tone(rx, 600.0, 2000);
    append_silence(rx, 1200);

    // ② AM @ 1200 Hz，调制 60 Hz，0.25 s
    marks.push_back(1200.0f);
    for (std::size_t i = 0; i < 2000; ++i) {
        const double t = static_cast<double>(i) / kFs;
        rx.push_back(static_cast<float>((1.0 + 0.7 * std::sin(2.0 * kPi * 60.0 * t)) *
                                        std::sin(2.0 * kPi * 1200.0 * t)));
    }
    append_silence(rx, 1200);

    // ③ FM @ 2000 Hz，调制 80 Hz，β=2，0.25 s
    marks.push_back(2000.0f);
    for (std::size_t i = 0; i < 2000; ++i) {
        const double t = static_cast<double>(i) / kFs;
        rx.push_back(static_cast<float>(
            std::sin(2.0 * kPi * 2000.0 * t + 2.0 * std::sin(2.0 * kPi * 80.0 * t))));
    }
    append_silence(rx, 1200);

    // ④ 2-FSK @ 3000±150 Hz，200 baud，40 个随机比特，0.2 s
    marks.push_back(3000.0f);
    std::vector<std::uint8_t> fsk_bits(40);
    {
        std::mt19937              rng(2026);
        std::bernoulli_distribution coin(0.5);
        for (auto& b : fsk_bits) b = coin(rng) ? 1u : 0u;
    }
    {
        const std::size_t sps = static_cast<std::size_t>(kFs / 200.0);
        double            phase = 0.0;
        for (std::uint8_t bit : fsk_bits) {
            const double f = bit ? 3150.0 : 2850.0;
            for (std::size_t i = 0; i < sps; ++i) {
                rx.push_back(static_cast<float>(0.5 * std::sin(phase)));
                phase += 2.0 * kPi * f / kFs;
            }
        }
    }

    my::log::info("采集: {} 个样本（{:.2f} s @ {:.0f} Hz）", rx.size(), rx.size() / kFs, kFs);

    // -------------------------------------------------------------------------
    // 2. 频谱：看看这段里都有什么
    // -------------------------------------------------------------------------
    auto spec = my::dsp::welch(rx, kFs, 1024, 0.5f, my::dsp::Window::Hann);
    if (!spec) {
        my::log::error("PSD 失败: {}", spec.status().to_string());
        return 1;
    }
    my::log::info("噪声底 {:.1f} dBFS，最强峰 {:.0f} Hz（{:.1f} dBFS），SNR≈{:.1f} dB",
                  spec.value().noise_floor_db, spec.value().peak_freq, spec.value().peak_db,
                  my::dsp::estimate_snr_db(spec.value()));

    const auto peaks = my::dsp::find_peaks(spec.value(), 8.0f, 100.0, 6);
    for (const auto& p : peaks) {
        my::log::info("  峰: {:7.1f} Hz  {:6.1f} dBFS", p.freq, p.db);
    }

    // -------------------------------------------------------------------------
    // 3. 检测：找突发
    // -------------------------------------------------------------------------
    const auto bursts = my::dsp::detect_bursts(rx, 128, 10.0f, 0);
    my::log::info("检测到 {} 个突发", bursts.size());
    if (bursts.size() != marks.size()) {
        my::log::warn("突发数（{}）和预期（{}）不一致，下面按顺序对照", bursts.size(), marks.size());
    }

    // -------------------------------------------------------------------------
    // 4. 识别：每个突发算特征并判调制
    // -------------------------------------------------------------------------
    my::log::info("{:<4} {:>8} {:>9} {:>7} {:>6} {:>7}  {:<8}", "#", "begin", "中心(Hz)",
                  "SNR", "cv", "ifs", "判定");
    std::vector<Samples> burst_signals;
    for (std::size_t i = 0; i < bursts.size(); ++i) {
        const auto& b = bursts[i];
        Samples     seg(rx.begin() + static_cast<std::ptrdiff_t>(b.begin),
                        rx.begin() + static_cast<std::ptrdiff_t>(b.end));
        burst_signals.push_back(seg);

        auto f = my::dsp::analyze(seg, kFs);
        if (!f) {
            my::log::warn("  #{} 特征失败: {}", i, f.status().to_string());
            continue;
        }
        const auto c = my::dsp::classify(f.value());
        my::log::info("{:<4} {:>8} {:>9.0f} {:>7.1f} {:>6.3f} {:>7.3f}  {:<8} ({:.2f})", i,
                      b.begin, f.value().center_freq_hz, f.value().snr_db, f.value().envelope_cv,
                      f.value().inst_freq_std, my::dsp::modulation_name(c.modulation),
                      c.confidence);
    }

    // -------------------------------------------------------------------------
    // 5. 解调：最后的 FSK 突发
    // -------------------------------------------------------------------------
    if (!burst_signals.empty()) {
        const Samples& fsk = burst_signals.back();
        const double   sps = kFs / 200.0;
        (void)sps;
        auto bits = my::dsp::demod_fsk(fsk, kFs, 200.0, 3150.0, 2850.0);
        if (!bits) {
            my::log::error("FSK 解调失败: {}", bits.status().to_string());
        } else {
            const std::size_t n = std::min(bits.value().size(), fsk_bits.size());
            std::size_t       errors = 0;
            for (std::size_t i = 0; i < n; ++i) {
                if (bits.value()[i] != fsk_bits[i]) ++errors;
            }
            my::log::info("FSK 解调: {} 个比特，与生成时对比错误 {} 个（BER = {:.3f}）",
                          bits.value().size(), errors,
                          n ? static_cast<double>(errors) / static_cast<double>(n) : 1.0);
        }
    }

    my::log::shutdown();
    return 0;
}
