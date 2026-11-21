#pragma once
// =============================================================================
//  my::codec —— 校验与编码：CRC / hex / base64 / COBS
//
//  为什么放在 core：串口、网口、日志、配置都要用这几样，而它们只依赖
//  Status/Result，没有任何三方依赖；header-only（全部 inline），板端交叉编译
//  直接 include 即可，不需要为它单独链一个 .a。
//
//  ── 各 CRC 变体的确切参数（不要笼统地说"CRC16 就是 CCITT"）──────────────────
//    函数            poly         init        refin  refout  xorout      标准名
//    crc16_ccitt     0x1021       0xFFFF      false  false   0x0000      CRC-16/CCITT-FALSE
//    crc16_modbus    0x8005(*)    0xFFFF      true   true    0x0000      CRC-16/MODBUS
//    crc32_ieee      0x04C11DB7(*) 0xFFFFFFFF true   true    0xFFFFFFFF  CRC-32/ISO-HDLC
//      (*) 反射实现，查表用的是位反转后的 0xA001 / 0xEDB88320；
//          "poly" 一列写的是规范里的原始多项式，便于和标准文档对表。
//    校验值（输入 "123456789"）：0x29B1 / 0x4B37 / 0xCBF43926。
//
//  ── 错误约定（与全库一致：对外 API 不抛异常）──────────────────────────────
//    * `data == nullptr && size > 0` 是**违反前置条件**（调用方的编程错误，
//      不是运行期可恢复错误），一律不碰空指针：
//        - noexcept 版本（crc* / update）返回 0 / 忽略本次调用；
//        - Result 版本返回 InvalidArgument；
//        - Status 版本返回 InvalidArgument；
//        - 返回 std::string / std::vector 的编码函数没有别的报错通道，返回空对象
//          （注释里单独写明）。
//    * 解码失败一律**不改 out**：先整体校验，校验通过后才写 out。
//    * 不做动态分配的函数（CRC、*_into）一次都不分配；`cobs_encode` /
//      `cobs_decode` 只在构造返回值/输出时分配一次。
// =============================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "my_cpp_libs/core/status.hpp"

namespace my::codec {

namespace detail {

// -----------------------------------------------------------------------------
//  CRC 查表
//
//  表在**编译期**生成（C++17 inline constexpr）：每个 TU 共享同一份只读常量，
//  不在函数里每次调用重新构造。256 项 × 2B/4B 的表，对 Zynq 的 L1 是可接受的
//  取舍（逐位算法慢约 8 倍，串口 115200 时是肉眼可见的差别）。
// -----------------------------------------------------------------------------

/// poly 0x1021 / refin=false 的 MSB-first 表（CRC-16/CCITT-FALSE 用）
constexpr std::array<std::uint16_t, 256> make_crc16_ccitt_table() noexcept {
    std::array<std::uint16_t, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        std::uint32_t crc = static_cast<std::uint32_t>(i) << 8;  // 数据字节对齐到高 8 位
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x8000u) != 0u) {
                crc = ((crc << 1) ^ 0x1021u) & 0xFFFFu;
            } else {
                crc = (crc << 1) & 0xFFFFu;
            }
        }
        table[i] = static_cast<std::uint16_t>(crc);
    }
    return table;
}

/// poly 0x8005 / refin=true 的表（位反转多项式 0xA001；CRC-16/MODBUS 用）
constexpr std::array<std::uint16_t, 256> make_crc16_modbus_table() noexcept {
    std::array<std::uint16_t, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        std::uint32_t crc = static_cast<std::uint32_t>(i);
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 1u) != 0u) {
                crc = (crc >> 1) ^ 0xA001u;
            } else {
                crc = crc >> 1;
            }
        }
        table[i] = static_cast<std::uint16_t>(crc);
    }
    return table;
}

/// poly 0x04C11DB7 / refin=true 的表（位反转多项式 0xEDB88320；CRC-32/ISO-HDLC 用）
constexpr std::array<std::uint32_t, 256> make_crc32_ieee_table() noexcept {
    std::array<std::uint32_t, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        std::uint32_t crc = static_cast<std::uint32_t>(i);
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 1u) != 0u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc = crc >> 1;
            }
        }
        table[i] = crc;
    }
    return table;
}

inline constexpr std::array<std::uint16_t, 256> kCrc16CcittTable = make_crc16_ccitt_table();
inline constexpr std::array<std::uint16_t, 256> kCrc16ModbusTable = make_crc16_modbus_table();
inline constexpr std::array<std::uint32_t, 256> kCrc32IeeeTable = make_crc32_ieee_table();

/// 逐字节推进一个"裸寄存器值"（不含 xorout 的那次取反）。
inline std::uint16_t crc16_ccitt_update(std::uint16_t state, const std::uint8_t* data,
                                        std::size_t size) noexcept {
    for (std::size_t i = 0; i < size; ++i) {
        const std::size_t idx = static_cast<std::size_t>((state >> 8) ^ data[i]) & 0xFFu;
        state = static_cast<std::uint16_t>(state << 8) ^ kCrc16CcittTable[idx];
    }
    return state;
}

inline std::uint16_t crc16_modbus_update(std::uint16_t state, const std::uint8_t* data,
                                         std::size_t size) noexcept {
    for (std::size_t i = 0; i < size; ++i) {
        const std::size_t idx = static_cast<std::size_t>(state ^ data[i]) & 0xFFu;
        state = static_cast<std::uint16_t>(state >> 8) ^ kCrc16ModbusTable[idx];
    }
    return state;
}

inline std::uint32_t crc32_ieee_update(std::uint32_t state, const std::uint8_t* data,
                                       std::size_t size) noexcept {
    for (std::size_t i = 0; i < size; ++i) {
        const std::size_t idx = static_cast<std::size_t>(state ^ data[i]) & 0xFFu;
        state = (state >> 8) ^ kCrc32IeeeTable[idx];
    }
    return state;
}

// -----------------------------------------------------------------------------
//  hex / base64 的字符映射
// -----------------------------------------------------------------------------

/// '0'-'9' / 'a'-'f' / 'A'-'F' → 0..15，其余返回 -1
constexpr int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline constexpr char kBase64Std[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
inline constexpr char kBase64Url[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/// 标准字母表与 url-safe 字母表都接受（'+'/'/' 与 '-'/'_' 等价）
constexpr int base64_value(char c) noexcept {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/// ASCII 空白：解码时全部跳过（换行/缩进过的 base64 很常见）
constexpr bool is_ascii_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// -----------------------------------------------------------------------------
//  COBS 核心（vector 版本与 *_into 版本共用在这里，保证两者逐字节一致）
// -----------------------------------------------------------------------------

/// 编码后的精确长度（含末尾 0x00 分隔符）。src 在 size==0 时可以为空指针。
inline std::size_t cobs_encoded_size(const std::uint8_t* src, std::size_t size) noexcept {
    std::size_t need = 2;  // 首块码字 + 末尾分隔符
    std::size_t run = 0;   // 当前块已经累了的非零字节数
    for (std::size_t i = 0; i < size; ++i) {
        if (src[i] == 0x00) {
            ++need;  // 0x00 被替换成一个新的码字
            run = 0;
        } else {
            ++need;  // 数据字节本身
            if (++run == 254) {  // 码字最大 0xFF = 254 个数据字节，满了必须开新块
                ++need;
                run = 0;
            }
        }
    }
    return need;
}

/// 按 cobs_encoded_size() 算出的长度写 dst（调用方保证 dst 足够，且 size==0 时可为空）。
inline void cobs_encode_unchecked(const std::uint8_t* src, std::size_t size,
                                  std::uint8_t* dst) noexcept {
    std::size_t code_pos = 0;     // 当前块码字在 dst 里的位置
    std::uint8_t code = 1;        // 当前块码字 = 数据字节数 + 1
    std::size_t pos = 0;
    dst[pos++] = 0x00;            // 第一个码字占位，块满/遇 0 时回填
    for (std::size_t i = 0; i < size; ++i) {
        if (src[i] == 0x00) {
            dst[code_pos] = code;
            code = 1;
            code_pos = pos;
            dst[pos++] = 0x00;    // 新块的码字占位
            continue;
        }
        dst[pos++] = src[i];
        ++code;
        if (code == 0xFFu) {
            dst[code_pos] = code;
            code = 1;
            code_pos = pos;
            dst[pos++] = 0x00;
        }
    }
    dst[code_pos] = code;
    dst[pos] = 0x00;              // 帧分隔符（本库的约定：encode 结果含它）
}

/// 校验 COBS 帧并算出解码后的字节数。
/// 允许带/不带末尾 0x00；帧内出现 0x00（分隔符在中间）、块内数据不够（截断）、
/// 空帧都算 ParseError。返回 false 时 decoded 无意义。
inline bool cobs_check_frame(const std::uint8_t* data, std::size_t size,
                             std::size_t& decoded) noexcept {
    decoded = 0;
    if (size == 0) return false;  // 连码字都没有：截断
    std::size_t end = size;
    if (data[end - 1] == 0x00) --end;  // 末尾分隔符可有可无
    if (end == 0) return false;        // 只有分隔符：同样是没有码字的截断
    std::size_t pos = 0;
    while (pos < end) {
        const std::uint8_t code = data[pos++];
        if (code == 0x00) return false;  // 码字位置出现 0x00 = 分隔符跑到帧中间
        const std::size_t run = static_cast<std::size_t>(code) - 1;
        if (end - pos < run) return false;  // 块内数据不够
        decoded += run;
        pos += run;
        if (code != 0xFF && pos < end) ++decoded;  // 块尾隐含的那个 0x00
    }
    return true;
}

/// 按 cobs_check_frame() 校验过的帧写 dst（调用方保证 dst 能装下 decoded 字节；
/// decoded==0 时 dst 可以是空指针，函数不会解引用）。
inline void cobs_decode_unchecked(const std::uint8_t* data, std::size_t size,
                                  std::uint8_t* dst) noexcept {
    const std::size_t end = (size != 0 && data[size - 1] == 0x00) ? size - 1 : size;
    std::size_t pos = 0;
    while (pos < end) {
        const std::uint8_t code = data[pos++];
        const std::size_t run = static_cast<std::size_t>(code) - 1;
        for (std::size_t i = 0; i < run; ++i) *dst++ = data[pos++];
        if (code != 0xFF && pos < end) *dst++ = 0x00;
    }
}

}  // namespace detail

// -----------------------------------------------------------------------------
//  CRC —— 一次性接口
//
//  data==nullptr && size>0 是违反前置条件：这些函数是 noexcept 的，没有报错
//  通道，返回 0（**不是** init）——返回值 0 在业务里不该被当成合法校验值。
// -----------------------------------------------------------------------------

/// CRC-16/CCITT-FALSE：poly 0x1021, init 0xFFFF, refin=false, refout=false, xorout=0。
/// "123456789" → 0x29B1。init 是寄存器初值（refin=false，不做位反转）。
inline std::uint16_t crc16_ccitt(const void* data, std::size_t size,
                                 std::uint16_t init = 0xFFFF) noexcept {
    if (data == nullptr) {
        return size == 0 ? init : 0;  // size>0 且空指针：违反前置条件，返回 0
    }
    return detail::crc16_ccitt_update(init, static_cast<const std::uint8_t*>(data), size);
}

/// CRC-16/MODBUS：poly 0x8005(反射实现，表用 0xA001), init 0xFFFF,
/// refin=true, refout=true, xorout=0。"123456789" → 0x4B37。
/// init 是**未经输出反射/取反**的寄存器初值（本变体 xorout=0，所以 init 就是终值的起点）。
inline std::uint16_t crc16_modbus(const void* data, std::size_t size,
                                  std::uint16_t init = 0xFFFF) noexcept {
    if (data == nullptr) {
        return size == 0 ? init : 0;  // size>0 且空指针：违反前置条件，返回 0
    }
    return detail::crc16_modbus_update(init, static_cast<const std::uint8_t*>(data), size);
}

/// CRC-32/ISO-HDLC（以太网 / zlib / gzip / PNG）：poly 0x04C11DB7(反射 0xEDB88320),
/// init 0xFFFFFFFF, refin=true, refout=true, xorout=0xFFFFFFFF。
/// "123456789" → 0xCBF43926；空输入 → 0x00000000（init 与 xorout 互相抵消）。
inline std::uint32_t crc32_ieee(const void* data, std::size_t size) noexcept {
    // nullptr && size==0 时 init 与 xorout 抵消，同样是 0，所以这里直接返回 0。
    if (data == nullptr) return 0u;
    std::uint32_t state = 0xFFFFFFFFu;  // init
    state = detail::crc32_ieee_update(state, static_cast<const std::uint8_t*>(data), size);
    return state ^ 0xFFFFFFFFu;         // xorout
}

// -----------------------------------------------------------------------------
//  CRC —— 增量版本（串口/网口按块喂）
//
//  用法：
//      my::codec::Crc32 c;
//      c.update(chunk, n);
//      const std::uint32_t v = c.value();   // 不结束计算，可继续 update
//      c.reset();                           // 回到初始状态，可复用
//  约定：update(nullptr, size) 且 size>0 是违反前置条件，忽略本次调用（noexcept，
//  没有报错通道）；update(nullptr, 0) 合法。
// -----------------------------------------------------------------------------

/// 增量 CRC-32/ISO-HDLC（参数见 crc32_ieee）
class Crc32 {
public:
    Crc32() noexcept = default;

    /// 回到 init=0xFFFFFFFF 的初始状态
    void reset() noexcept { state_ = 0xFFFFFFFFu; }

    void update(const void* data, std::size_t size) noexcept {
        if (size == 0 || data == nullptr) return;
        state_ = detail::crc32_ieee_update(state_, static_cast<const std::uint8_t*>(data), size);
    }

    /// 当前校验值（每次调用都会做 xorout，不改变内部状态）
    std::uint32_t value() const noexcept { return state_ ^ 0xFFFFFFFFu; }

private:
    std::uint32_t state_ = 0xFFFFFFFFu;  // 裸寄存器值，不含 xorout
};

/// 增量 CRC-16/CCITT-FALSE（poly 0x1021, refin/refout=false, xorout=0）
class Crc16Ccitt {
public:
    /// init 是寄存器初值；默认 0xFFFF（CCITT-FALSE）。构造后即可 update。
    explicit Crc16Ccitt(std::uint16_t init = 0xFFFFu) noexcept : init_(init), state_(init) {}

    /// 回到**构造时给定的** init
    void reset() noexcept { state_ = init_; }

    void update(const void* data, std::size_t size) noexcept {
        if (size == 0 || data == nullptr) return;
        state_ = detail::crc16_ccitt_update(state_, static_cast<const std::uint8_t*>(data), size);
    }

    /// xorout=0、refout=false：裸寄存器值就是校验值
    std::uint16_t value() const noexcept { return state_; }

private:
    std::uint16_t init_ = 0xFFFFu;
    std::uint16_t state_ = 0xFFFFu;
};

/// 增量 CRC-16/MODBUS（反射 poly 0xA001, init 0xFFFF, refin/refout=true, xorout=0）
class Crc16Modbus {
public:
    /// init 是寄存器初值；默认 0xFFFF。同理，构造时可用别的初值（如 0x0000 = CRC-16/ARC）。
    explicit Crc16Modbus(std::uint16_t init = 0xFFFFu) noexcept : init_(init), state_(init) {}

    /// 回到**构造时给定的** init
    void reset() noexcept { state_ = init_; }

    void update(const void* data, std::size_t size) noexcept {
        if (size == 0 || data == nullptr) return;
        state_ = detail::crc16_modbus_update(state_, static_cast<const std::uint8_t*>(data), size);
    }

    /// xorout=0、refout=true：本类的内部状态已经按反射算法推进，直接返回
    std::uint16_t value() const noexcept { return state_; }

private:
    std::uint16_t init_ = 0xFFFFu;
    std::uint16_t state_ = 0xFFFFu;
};

// -----------------------------------------------------------------------------
//  hex
// -----------------------------------------------------------------------------

/// 字节 → 十六进制文本。lower 默认小写；upper=true 用大写 A-F。
/// 空输入（data 可为 nullptr）→ 空串；data==nullptr && size>0（违反前置条件）→ 空串。
inline std::string hex_encode(const void* data, std::size_t size, bool upper = false) {
    if (data == nullptr) return {};  // size>0 属于违反前置条件；size==0 合法
    if (size > std::numeric_limits<std::size_t>::max() / 2) return {};  // 极端尺寸下避免 size*2 回绕
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    const auto* src = static_cast<const std::uint8_t*>(data);
    std::string out(size * 2, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        out[i * 2] = digits[src[i] >> 4];
        out[i * 2 + 1] = digits[src[i] & 0x0Fu];
    }
    return out;
}

/// 十六进制文本 → 字节。大小写都接受；**长度为奇数**或**含非法字符** → ParseError。
/// 失败时 out 不变（先整体校验，再写 out）；成功时空串会把 out 清空。
inline Status hex_decode(std::string_view text, std::vector<std::uint8_t>& out) {
    if ((text.size() & 1u) != 0u) {
        return Status(Errc::ParseError, "hex: 长度为奇数，无法按字节切分");
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (detail::hex_value(text[i]) < 0) {
            return Status(Errc::ParseError, "hex: 含非十六进制字符");
        }
    }
    out.clear();
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = detail::hex_value(text[i]);
        const int lo = detail::hex_value(text[i + 1]);
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return Status::success();
}

// -----------------------------------------------------------------------------
//  base64（RFC 4648）
// -----------------------------------------------------------------------------

/// 标准字母表（url_safe=true 时用 '-'/'_' 代替 '+'/'/'）。
/// padding=true 时补 '=' 到 4 的倍数，false 时一个 '=' 都不写。
/// data==nullptr && size>0（违反前置条件）→ 空串。
inline std::string base64_encode(const void* data, std::size_t size, bool url_safe = false,
                                 bool padding = true) {
    if (data == nullptr) return {};  // size>0 属于违反前置条件；size==0 合法
    if (size > std::numeric_limits<std::size_t>::max() / 4) return {};  // 极端尺寸下避免长度回绕
    const char* table = url_safe ? detail::kBase64Url : detail::kBase64Std;
    const auto* src = static_cast<const std::uint8_t*>(data);

    const std::size_t full = size / 3;
    const std::size_t rest = size % 3;
    std::size_t chars = full * 4;
    if (rest == 1) {
        chars += padding ? 4u : 2u;
    } else if (rest == 2) {
        chars += padding ? 4u : 3u;
    }

    std::string out(chars, '\0');  // 长度已知：一次分配
    std::size_t o = 0;
    for (std::size_t i = 0; i < full; ++i) {
        const std::uint32_t v = (static_cast<std::uint32_t>(src[i * 3]) << 16) |
                                (static_cast<std::uint32_t>(src[i * 3 + 1]) << 8) |
                                static_cast<std::uint32_t>(src[i * 3 + 2]);
        out[o++] = table[(v >> 18) & 0x3Fu];
        out[o++] = table[(v >> 12) & 0x3Fu];
        out[o++] = table[(v >> 6) & 0x3Fu];
        out[o++] = table[v & 0x3Fu];
    }
    if (rest == 1) {
        const std::uint32_t v = static_cast<std::uint32_t>(src[full * 3]) << 16;
        out[o++] = table[(v >> 18) & 0x3Fu];
        out[o++] = table[(v >> 12) & 0x3Fu];
        if (padding) {
            out[o++] = '=';
            out[o++] = '=';
        }
    } else if (rest == 2) {
        const std::uint32_t v = (static_cast<std::uint32_t>(src[full * 3]) << 16) |
                                (static_cast<std::uint32_t>(src[full * 3 + 1]) << 8);
        out[o++] = table[(v >> 18) & 0x3Fu];
        out[o++] = table[(v >> 12) & 0x3Fu];
        out[o++] = table[(v >> 6) & 0x3Fu];
        if (padding) out[o++] = '=';
    }
    return out;
}

/// base64 文本 → 字节。
///   接受：标准与 url-safe 两种字母表、任意 ASCII 空白（自动跳过）、可缺 padding。
///   拒绝（ParseError）：非法字符、padding 出现在中间/数量不对、有效长度 %4 == 1。
/// 失败时 out 不变（先整体校验，再写 out）。
/// 注意：不校验最后一组未使用的低位是否为零（宽松解码，"Zh==" 与 "Zg==" 都解成 'f'）。
inline Status base64_decode(std::string_view text, std::vector<std::uint8_t>& out) {
    // 第一遍：只校验 + 数长度，不碰 out
    std::size_t significant = 0;  // 非空白字符数（含 '='）
    std::size_t pad = 0;          // 末尾连续的 '=' 个数
    bool saw_pad = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (detail::is_ascii_space(c)) continue;
        if (c == '=') {
            saw_pad = true;
            ++pad;
            ++significant;
            if (pad > 2) {
                return Status(Errc::ParseError, "base64: '=' 多于 2 个");
            }
            continue;
        }
        if (saw_pad) {
            return Status(Errc::ParseError, "base64: '=' 之后还有有效字符");
        }
        if (detail::base64_value(c) < 0) {
            return Status(Errc::ParseError, "base64: 含非字母表字符");
        }
        ++significant;
    }

    const std::size_t core = significant - pad;  // 真正的数据字符数
    const std::size_t rem = core % 4;
    if (rem == 1) {
        return Status(Errc::ParseError, "base64: 有效长度为 4k+1，无法解码");
    }
    if (pad > 0) {
        if (rem == 0) {
            return Status(Errc::ParseError, "base64: 长度已是 4 的倍数却带了 '='");
        }
        if (pad != 4 - rem) {  // rem=2 需要 "=="，rem=3 需要 "="
            return Status(Errc::ParseError, "base64: '=' 数量与长度不匹配");
        }
    }

    // 第二遍：解码
    out.clear();
    out.reserve((core / 4) * 3 + (rem == 2 ? 1u : (rem == 3 ? 2u : 0u)));
    std::uint32_t acc = 0;
    int nbits = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (detail::is_ascii_space(c)) continue;
        if (c == '=') break;  // 校验阶段已保证 '=' 只会出现在末尾
        acc = ((acc << 6) | static_cast<std::uint32_t>(detail::base64_value(c))) & 0xFFFFFFu;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> nbits) & 0xFFu));
        }
    }
    return Status::success();
}

// -----------------------------------------------------------------------------
//  COBS（Consistent Overhead Byte Stuffing，Cheshire & Baker, SIGCOMM 1999）
//
//  **本库的约定：encode 的结果含末尾 0x00 分隔符**（组帧时直接发送即可），
//  decode 则带不带分隔符都收（方便处理"只给了 payload 段"的场景）。
//  编码把每个 0x00 替换成长度码，因此 encoded 里除首尾外不会再出现 0x00。
// -----------------------------------------------------------------------------

/// 编码结果长度的**上界**（含末尾 0x00 分隔符）：size + size/254 + 2。
/// 说明：教科书上常见的是 size + size/254 + 1，那是"不含帧分隔符"的口径；
/// 本库 encode 结果含分隔符，所以必须再 +1 —— 若按 1 的口径申请缓冲，
/// 每一次非空编码都会 OutOfRange。（0 长度输入也会输出 {0x01, 0x00} 共 2 字节。）
inline std::size_t cobs_max_encoded_size(std::size_t size) noexcept {
    const std::size_t extra = size / 254;
    const std::size_t max = std::numeric_limits<std::size_t>::max();
    if (size > max - 2 - extra) return max;  // 极端尺寸饱和，避免回绕成"小缓冲区"
    return size + extra + 2;
}

/// 编码（含末尾 0x00 分隔符）。data==nullptr && size>0（违反前置条件）→ 空 vector。
/// 只分配一次：按 detail::cobs_encoded_size() 的精确长度构造返回值。
inline std::vector<std::uint8_t> cobs_encode(const void* data, std::size_t size) {
    if (data == nullptr && size > 0) return {};
    const auto* src = static_cast<const std::uint8_t*>(data);
    std::vector<std::uint8_t> out(detail::cobs_encoded_size(src, size));  // 唯一一次分配
    detail::cobs_encode_unchecked(src, size, out.data());
    return out;
}

/// 解码（含/不含末尾 0x00 都可以）。
/// 非法帧 → ParseError，且**失败时 out 不变**；空帧（size==0）也是非法帧。
inline Status cobs_decode(const std::uint8_t* data, std::size_t size,
                          std::vector<std::uint8_t>& out) {
    if (data == nullptr && size > 0) {
        return Status(Errc::InvalidArgument, "cobs_decode: data 为空但 size > 0");
    }
    std::size_t decoded = 0;
    if (!detail::cobs_check_frame(data, size, decoded)) {
        return Status(Errc::ParseError, "cobs_decode: 非法帧（截断，或 0x00 出现在帧中间）");
    }
    out.clear();
    out.resize(decoded);  // 校验通过后才动 out：失败时 out 保持不变
    if (decoded != 0) {
        detail::cobs_decode_unchecked(data, size, out.data());
    }
    return Status::success();
}

/// 板端零分配编码：把编码结果（含分隔符）写进调用方缓冲，返回实际写入字节数。
///   data==nullptr && size>0        → InvalidArgument
///   dst==nullptr && cap>0          → InvalidArgument
///   cap < 精确所需长度             → OutOfRange（dst 一个字节都不写）
/// 需要的缓冲上限可用 cobs_max_encoded_size(size)。
///
/// 注意：失败路径也**不带格式化消息**（只有错误码），否则拼消息会堆分配，
/// 就违背了"零分配"的初衷；要人读的消息请用带 vector 的 cobs_encode/解码版本。
inline Result<std::size_t> cobs_encode_into(const void* data, std::size_t size, void* dst,
                                            std::size_t cap) {
    if (data == nullptr && size > 0) {
        return Result<std::size_t>(Status(Errc::InvalidArgument));
    }
    if (dst == nullptr && cap > 0) {
        return Result<std::size_t>(Status(Errc::InvalidArgument));
    }
    const auto* src = static_cast<const std::uint8_t*>(data);
    const std::size_t need = detail::cobs_encoded_size(src, size);
    if (cap < need) {
        return Result<std::size_t>(Status(Errc::OutOfRange));
    }
    detail::cobs_encode_unchecked(src, size, static_cast<std::uint8_t*>(dst));
    return need;
}

/// 板端零分配解码：把解码结果写进调用方缓冲，返回实际写入字节数。
///   data==nullptr && size>0        → InvalidArgument
///   dst==nullptr && cap>0          → InvalidArgument
///   非法帧                         → ParseError（dst 一个字节都不写）
///   cap < 精确所需长度             → OutOfRange（dst 一个字节都不写）
/// 与 cobs_encode_into 一样，失败路径只给错误码、不拼消息，保证一次都不分配。
inline Result<std::size_t> cobs_decode_into(const std::uint8_t* data, std::size_t size, void* dst,
                                            std::size_t cap) {
    if (data == nullptr && size > 0) {
        return Result<std::size_t>(Status(Errc::InvalidArgument));
    }
    if (dst == nullptr && cap > 0) {
        return Result<std::size_t>(Status(Errc::InvalidArgument));
    }
    std::size_t need = 0;
    if (!detail::cobs_check_frame(data, size, need)) {
        return Result<std::size_t>(Status(Errc::ParseError));
    }
    if (cap < need) {
        return Result<std::size_t>(Status(Errc::OutOfRange));
    }
    if (need != 0) {
        detail::cobs_decode_unchecked(data, size, static_cast<std::uint8_t*>(dst));
    }
    return need;
}

}  // namespace my::codec
