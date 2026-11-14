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

/// fp32 -> fp16（含简单舍入；溢出转 inf，过小转次正规数或 0）
inline std::uint16_t float_to_half(float value) noexcept {
    std::uint32_t x;
    std::memcpy(&x, &value, sizeof(x));

    const std::uint32_t sign = (x >> 16) & 0x8000u;
    const std::int32_t  e    = static_cast<std::int32_t>((x >> 23) & 0xFFu) - 127;
    const std::uint32_t m    = x & 0x7FFFFFu;

    if (e == 128) {  // inf / nan
        return static_cast<std::uint16_t>(sign | 0x7C00u | (m != 0 ? 0x200u : 0u));
    }
    if (e > 15) {  // 溢出
        return static_cast<std::uint16_t>(sign | 0x7C00u);
    }
    if (e >= -14) {  // 正规数
        std::uint32_t hm = m >> 13;
        if (m & 0x1000u) ++hm;  // 四舍五入
        std::uint32_t he = static_cast<std::uint32_t>(e + 15);
        if (hm == 0x400u) {
            hm = 0;
            ++he;
        }
        if (he >= 0x1Fu) return static_cast<std::uint16_t>(sign | 0x7C00u);
        return static_cast<std::uint16_t>(sign | (he << 10) | (hm & 0x3FFu));
    }

    // 次正规数（或下溢为 0）
    const std::int32_t shift = -14 - e;
    if (shift > 24) return static_cast<std::uint16_t>(sign);
    const std::uint32_t hm = (m | 0x800000u) >> (shift + 13);
    return static_cast<std::uint16_t>(sign | (hm & 0x3FFu));
}

}  // namespace my
