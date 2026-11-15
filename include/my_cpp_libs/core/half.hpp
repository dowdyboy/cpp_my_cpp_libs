#pragma once
// =============================================================================
//  IEEE 754 半精度（fp16）与单精度互转
//
//  为什么需要：不少训练框架导出的模型输入/输出是 fp16，而 C++ 侧统一按
//  float 处理最省事。这里给一对无依赖的转换函数，避免为此引入第三方库。
// =============================================================================

#include <cstdint>
#include <cstring>

namespace my {

/// fp16 -> fp32（正确处理次正规数、inf、nan）
inline float half_to_float(std::uint16_t h) noexcept {
    const std::uint32_t sign = (static_cast<std::uint32_t>(h) >> 15) & 0x1u;
    const std::uint32_t exp  = (static_cast<std::uint32_t>(h) >> 10) & 0x1Fu;
    const std::uint32_t mant = static_cast<std::uint32_t>(h) & 0x3FFu;

    std::uint32_t bits = 0;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign << 31;  // ±0
        } else {
            // 次正规数：规格化
            std::int32_t e = -1;
            std::uint32_t m = mant;
            do {
                ++e;
                m <<= 1;
            } while ((m & 0x400u) == 0);
            bits = (sign << 31) | (static_cast<std::uint32_t>(127 - 15 - e) << 23) |
                   ((m & 0x3FFu) << 13);
        }
    } else if (exp == 0x1F) {
        bits = (sign << 31) | 0x7F800000u | (mant << 13);  // inf / nan
    } else {
        bits = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
    }

    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

/// fp32 -> fp16（IEEE 754 round-to-nearest-even；溢出转 inf，过小转次正规数或 0）
///
/// 舍入规则与硬件（F16C / _cvtss_sh）、ONNX Runtime、ncnn 一致，因此跨后端比较
/// fp16 输出时不会因为"差 1 ULP"被误判成两个后端不一致。
inline std::uint16_t float_to_half(float value) noexcept {
    std::uint32_t x;
    std::memcpy(&x, &value, sizeof(x));

    const std::uint32_t sign = (x >> 16) & 0x8000u;
    const std::uint32_t w    = x & 0x7FFFFFFFu;  // 去掉符号位后的位模式

    // ---- inf / nan：结果保证是 quiet NaN，并保留尾数高位 ----
    if (w >= 0x7F800000u) {
        const std::uint32_t mant = w & 0x007FFFFFu;
        return static_cast<std::uint16_t>(sign | 0x7C00u |
                                          (mant != 0u ? (0x0200u | (mant >> 13)) : 0u));
    }

    // ---- <= 2^-25：小于最小次正规数的一半，RNE 一律舍到 0 ----
    //   （恰好 2^-25 是 0 与 2^-24 的中点，向偶数舍入 → 0；0x33000000 就是 2^-25）
    if (w <= 0x33000000u) {
        return static_cast<std::uint16_t>(sign);
    }

    // ---- 上溢：65520 = 65504 + 16 是"最大有限值"与 inf 的中点，RNE 到 inf ----
    if (w >= 0x477FF000u) {
        return static_cast<std::uint16_t>(sign | 0x7C00u);
    }

    const std::int32_t  e = static_cast<std::int32_t>((w >> 23) & 0xFFu) - 127;
    const std::uint32_t m = w & 0x007FFFFFu;

    if (e >= -14) {  // 正规数
        // 24 位有效数 -> 11 位（10 位尾数 + 进位），RNE
        const std::uint32_t mant24 = m | 0x800000u;
        std::uint32_t       hm     = (mant24 + 0x0FFFu + ((mant24 >> 13) & 1u)) >> 13;
        std::uint32_t       he     = static_cast<std::uint32_t>(e + 15);
        if (hm == 0x800u) {  // 尾数进位到下一位指数
            hm = 0;
            ++he;
        }
        return static_cast<std::uint16_t>(sign | (he << 10) | (hm & 0x3FFu));
    }

    // ---- 次正规数：e ∈ [-25, -15]，写成 k * 2^-24 的形式 ----
    //   值 = 1.m * 2^e = mant24 * 2^(e-23)；令 k = round(值 / 2^-24) = round(mant24 / 2^s)，
    //   s = -e - 1 ∈ [14, 24]。s 最大 24，右移量恒小于 32，不存在未定义行为。
    const std::uint32_t mant24 = m | 0x800000u;
    const std::uint32_t s      = static_cast<std::uint32_t>(-e - 1);
    const std::uint32_t half   = 1u << (s - 1u);
    const std::uint32_t k      = (mant24 + half - 1u + ((mant24 >> s) & 1u)) >> s;
    // k 最大会进到 1024：这是"最大的次正规数向上舍入"落到最小正规数 2^-14，
    // 不能再用 0x3FF 掩码（那样会变成 0）。
    if (k >= 0x400u) return static_cast<std::uint16_t>(sign | 0x0400u);
    return static_cast<std::uint16_t>(sign | (k & 0x3FFu));
}

}  // namespace my
