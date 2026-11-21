#include "test_framework.hpp"

#include <my_cpp_libs/core/codec.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

MYTEST_SUITE("codec");

namespace {

/// 0x00..0xFF 的完整字节表（COBS 的往返测试用）
std::vector<std::uint8_t> all_bytes() {
    std::vector<std::uint8_t> v(256);
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = static_cast<std::uint8_t>(i);
    return v;
}

std::vector<std::uint8_t> bytes(const char* text) {
    std::vector<std::uint8_t> v;
    for (const char* p = text; *p != '\0'; ++p) v.push_back(static_cast<std::uint8_t>(*p));
    return v;
}

/// out 是否还是初值（用于验证"失败时 out 不变"）
bool untouched(const std::vector<std::uint8_t>& v, std::uint8_t probe) {
    for (std::uint8_t b : v) {
        if (b != probe) return false;
    }
    return true;
}

}  // namespace

// -----------------------------------------------------------------------------
//  1. CRC 标准校验值
//
//  参考值来源：CRC 参数目录（Greg Cook 的 CRC catalogue / reveng 的 check 值），
//  这三个变体的 check 值在所有实现里都是共识：
//      CRC-16/CCITT-FALSE  "123456789" -> 0x29B1
//      CRC-16/MODBUS       "123456789" -> 0x4B37
//      CRC-32/ISO-HDLC     "123456789" -> 0xCBF43926
// -----------------------------------------------------------------------------
MYTEST_CASE(codec_crc_known_vectors) {
    const char*       text = "123456789";
    const std::size_t len  = 9;

    CHECK_EQ(my::codec::crc16_ccitt(text, len), static_cast<std::uint16_t>(0x29B1));
    CHECK_EQ(my::codec::crc16_modbus(text, len), static_cast<std::uint16_t>(0x4B37));
    CHECK_EQ(my::codec::crc32_ieee(text, len), static_cast<std::uint32_t>(0xCBF43926));

    // 空输入 = 与 init 相关的值：CCITT/MODBUS 的 init=0xFFFF；
    // CRC32 的 init 与 xorout 互相抵消所以是 0。
    CHECK_EQ(my::codec::crc16_ccitt(nullptr, 0), static_cast<std::uint16_t>(0xFFFF));
    CHECK_EQ(my::codec::crc16_modbus(nullptr, 0), static_cast<std::uint16_t>(0xFFFF));
    CHECK_EQ(my::codec::crc32_ieee(nullptr, 0), static_cast<std::uint32_t>(0x00000000));

    // init 参数：0x1021 + init 0x0000 就是 CRC-16/XMODEM（check 0x31C3）；
    //            反射 0xA001 + init 0x0000 就是 CRC-16/ARC（check 0xBB3D）。
    CHECK_EQ(my::codec::crc16_ccitt(text, len, 0x0000), static_cast<std::uint16_t>(0x31C3));
    CHECK_EQ(my::codec::crc16_modbus(text, len, 0x0000), static_cast<std::uint16_t>(0xBB3D));

    // data==nullptr 且 size>0：违反前置条件，noexcept 版本返回 0（绝不解引用）。
    CHECK_EQ(my::codec::crc16_ccitt(nullptr, 3), static_cast<std::uint16_t>(0));
    CHECK_EQ(my::codec::crc16_modbus(nullptr, 3), static_cast<std::uint16_t>(0));
    CHECK_EQ(my::codec::crc32_ieee(nullptr, 3), static_cast<std::uint32_t>(0));
}

// -----------------------------------------------------------------------------
//  2. 增量 CRC 与一次性结果必须一致（1/2/3/7 字节切块）
// -----------------------------------------------------------------------------
MYTEST_CASE(codec_crc_incremental_matches_oneshot) {
    std::vector<std::uint8_t> buf = all_bytes();
    for (const char* p = "hello"; *p != '\0'; ++p) buf.push_back(static_cast<std::uint8_t>(*p));
    CHECK_EQ(buf.size(), std::size_t{261});  // 故意不是分块大小的倍数

    const std::size_t chunks[] = {1, 2, 3, 7};
    for (std::size_t chunk : chunks) {
        my::codec::Crc32      crc32;
        my::codec::Crc16Ccitt ccitt;
        my::codec::Crc16Modbus modbus;
        for (std::size_t off = 0; off < buf.size(); off += chunk) {
            const std::size_t left = buf.size() - off;
            const std::size_t take = left < chunk ? left : chunk;
            crc32.update(buf.data() + off, take);
            ccitt.update(buf.data() + off, take);
            modbus.update(buf.data() + off, take);
        }
        CHECK_EQ(crc32.value(), my::codec::crc32_ieee(buf.data(), buf.size()));
        CHECK_EQ(ccitt.value(), my::codec::crc16_ccitt(buf.data(), buf.size()));
        CHECK_EQ(modbus.value(), my::codec::crc16_modbus(buf.data(), buf.size()));
    }

    // 初值：reset 之后的状态
    my::codec::Crc32       fresh32;
    my::codec::Crc16Ccitt  fresh_ccitt;
    my::codec::Crc16Modbus fresh_modbus;
    CHECK_EQ(fresh32.value(), static_cast<std::uint32_t>(0x00000000));  // init ^ xorout
    CHECK_EQ(fresh_ccitt.value(), static_cast<std::uint16_t>(0xFFFF));
    CHECK_EQ(fresh_modbus.value(), static_cast<std::uint16_t>(0xFFFF));

    // 复用：喂了一半再 reset，结果必须与一次性相同
    my::codec::Crc32 reuse;
    reuse.update(buf.data(), 10);
    reuse.reset();
    reuse.update(buf.data(), buf.size());
    CHECK_EQ(reuse.value(), my::codec::crc32_ieee(buf.data(), buf.size()));

    // 空块不改变状态
    const std::uint32_t before = reuse.value();
    reuse.update(nullptr, 0);
    reuse.update(buf.data(), 0);
    CHECK_EQ(reuse.value(), before);

    // 自定义 init 的增量类与一次性 init 参数一致
    my::codec::Crc16Ccitt xmodem(0x0000);
    xmodem.update("123456789", 9);
    CHECK_EQ(xmodem.value(), my::codec::crc16_ccitt("123456789", 9, 0x0000));
    xmodem.reset();
    CHECK_EQ(xmodem.value(), static_cast<std::uint16_t>(0x0000));
}

// -----------------------------------------------------------------------------
//  3. hex 往返 / 大小写 / 非法输入
// -----------------------------------------------------------------------------
MYTEST_CASE(codec_hex_roundtrip) {
    std::vector<std::uint8_t> out;

    // 空输入
    CHECK_EQ(my::codec::hex_encode(nullptr, 0), std::string());
    CHECK_OK(my::codec::hex_decode("", out));
    CHECK(out.empty());

    const std::vector<std::uint8_t> data = {0x00, 0xFF, 0x10};
    CHECK_EQ(my::codec::hex_encode(data.data(), data.size()), std::string("00ff10"));
    CHECK_EQ(my::codec::hex_encode(data.data(), data.size(), true), std::string("00FF10"));

    // 解码接受小写/大写/混合
    CHECK_OK(my::codec::hex_decode("00ff10", out));
    CHECK(out == data);
    CHECK_OK(my::codec::hex_decode("00FF10", out));
    CHECK(out == data);
    CHECK_OK(my::codec::hex_decode("00fF10", out));
    CHECK(out == data);

    // 编码 -> 解码往返
    CHECK_OK(my::codec::hex_decode(my::codec::hex_encode(data.data(), data.size(), true), out));
    CHECK(out == data);

    // 失败时 out 不变：先放一个哨兵
    out = {0xAA, 0xBB};
    const std::vector<std::uint8_t> sentinel = out;

    // 奇数长度
    for (const char* bad : {"0", "abc", "00f"}) {
        const my::Status st = my::codec::hex_decode(bad, out);
        CHECK_EQ(st.code(), my::Errc::ParseError);
        CHECK(out == sentinel);
    }
    // 非法字符（含空格：hex 不做空白处理）
    for (const char* bad : {"0g", "zz", "0x10", "00 11"}) {
        const my::Status st = my::codec::hex_decode(bad, out);
        CHECK_EQ(st.code(), my::Errc::ParseError);
        CHECK(out == sentinel);
    }

    // data==nullptr && size>0（违反前置条件）→ 空串
    CHECK_EQ(my::codec::hex_encode(nullptr, 3), std::string());
}

// -----------------------------------------------------------------------------
//  4. base64：RFC 4648 §10 的测试向量 + 二进制往返 + 双字母表
// -----------------------------------------------------------------------------
MYTEST_CASE(codec_base64_rfc4648_vectors) {
    struct Vector {
        const char* plain;
        const char* padded;
        const char* no_pad;
    };
    // RFC 4648 §10（"Base 64 Encoding" 的测试向量）
    const Vector vectors[] = {
        {"", "", ""},
        {"f", "Zg==", "Zg"},
        {"fo", "Zm8=", "Zm8"},
        {"foo", "Zm9v", "Zm9v"},
        {"foob", "Zm9vYg==", "Zm9vYg"},
        {"fooba", "Zm9vYmE=", "Zm9vYmE"},
        {"foobar", "Zm9vYmFy", "Zm9vYmFy"},
    };

    for (const Vector& v : vectors) {
        const std::size_t n = std::char_traits<char>::length(v.plain);
        CHECK_EQ(my::codec::base64_encode(v.plain, n), std::string(v.padded));
        CHECK_EQ(my::codec::base64_encode(v.plain, n, false, false), std::string(v.no_pad));

        const std::vector<std::uint8_t> expect(v.plain, v.plain + n);
        std::vector<std::uint8_t>       back;
        CHECK_OK(my::codec::base64_decode(v.padded, back));
        CHECK(back == expect);
        CHECK_OK(my::codec::base64_decode(v.no_pad, back));  // 缺 padding 也接受
        CHECK(back == expect);
    }

    // 含 0x00 / 0xFF 的二进制往返
    const std::vector<std::uint8_t> bin = {0x00, 0xFF, 0x00, 0x7F, 0x80, 0x01, 0xFE};
    const std::string               encoded = my::codec::base64_encode(bin.data(), bin.size());
    std::vector<std::uint8_t>       out;
    CHECK_OK(my::codec::base64_decode(encoded, out));
    CHECK(out == bin);

    // url-safe 字母表（'+'/'/' -> '-'/'_'）+ 不补 '='
    const std::vector<std::uint8_t> url_bin = {0xFB, 0xFF};
    CHECK_EQ(my::codec::base64_encode(url_bin.data(), url_bin.size()), std::string("+/8="));
    CHECK_EQ(my::codec::base64_encode(url_bin.data(), url_bin.size(), true), std::string("-_8="));
    CHECK_EQ(my::codec::base64_encode(url_bin.data(), url_bin.size(), true, false), std::string("-_8"));

    // 解码对两种字母表都接受（'+' 与 '-'、'/' 与 '_' 等价）
    for (const char* both : {"+/8=", "-_8=", "+/8", "-_8"}) {
        CHECK_OK(my::codec::base64_decode(both, out));
        CHECK(out == url_bin);
    }

    // ASCII 空白被忽略（换行/缩进的 base64 很常见）
    CHECK_OK(my::codec::base64_decode("Zm9v YmFy\r\n", out));
    CHECK_EQ(std::string(out.begin(), out.end()), std::string("foobar"));
    CHECK_OK(my::codec::base64_decode("Zg ==\n", out));
    CHECK_EQ(std::string(out.begin(), out.end()), std::string("f"));

    // 非法字符 / 长度：ParseError 且 out 不变
    out = {0xAA};
    const std::vector<std::uint8_t> sentinel = out;
    const char* bad[] = {
        "****",       // 非法字符
        "Z",          // 有效长度 4k+1
        "Zg=",        // padding 数量不对（2 个字符要 "=="）
        "Zm9v=",      // 已满 4 个字符不该有 '='
        "Zg===",      // '=' 多于 2 个
        "Z=g=",       // '=' 之后还有有效字符
        "Zm9vYmFy=",  // 长度 9：不是合法 base64
    };
    for (const char* text : bad) {
        const my::Status st = my::codec::base64_decode(text, out);
        CHECK_EQ(st.code(), my::Errc::ParseError);
        CHECK(out == sentinel);
    }

    // data==nullptr && size>0（违反前置条件）→ 空串
    CHECK_EQ(my::codec::base64_encode(nullptr, 3), std::string());
}

// -----------------------------------------------------------------------------
//  5. COBS 标准向量 / 全字节往返 / 254-255 边界 / 上界 / 非法帧
//
//  参考：Cheshire & Baker, "Consistent Overhead Byte Stuffing", SIGCOMM 1999。
//  本库约定 encode 结果含末尾 0x00 分隔符，所以给的标准例子末尾都带 0x00。
// -----------------------------------------------------------------------------
MYTEST_CASE(codec_cobs_vectors) {
    // {0x00} -> {0x01, 0x01, 0x00}
    {
        const std::vector<std::uint8_t> in     = {0x00};
        const std::vector<std::uint8_t> expect = {0x01, 0x01, 0x00};
        const auto                      enc    = my::codec::cobs_encode(in.data(), in.size());
        CHECK(enc == expect);
        std::vector<std::uint8_t> back;
        CHECK_OK(my::codec::cobs_decode(enc.data(), enc.size(), back));
        CHECK(back == in);
    }
    // {0x11,0x22,0x00,0x33} -> {0x03,0x11,0x22,0x02,0x33,0x00}
    {
        const std::vector<std::uint8_t> in     = {0x11, 0x22, 0x00, 0x33};
        const std::vector<std::uint8_t> expect = {0x03, 0x11, 0x22, 0x02, 0x33, 0x00};
        const auto                      enc    = my::codec::cobs_encode(in.data(), in.size());
        CHECK(enc == expect);
        std::vector<std::uint8_t> back;
        CHECK_OK(my::codec::cobs_decode(enc.data(), enc.size(), back));
        CHECK(back == in);
    }
    // 空输入 -> {0x01, 0x00}（"没有数据"的块 + 分隔符）
    {
        const auto enc = my::codec::cobs_encode(nullptr, 0);
        CHECK(enc == (std::vector<std::uint8_t>{0x01, 0x00}));
        std::vector<std::uint8_t> back = {0xAA};
        CHECK_OK(my::codec::cobs_decode(enc.data(), enc.size(), back));
        CHECK(back.empty());
    }
    // 0..255 全字节往返
    {
        const std::vector<std::uint8_t> all = all_bytes();
        const auto                      enc = my::codec::cobs_encode(all.data(), all.size());
        CHECK(enc.size() <= my::codec::cobs_max_encoded_size(all.size()));
        // 除末尾分隔符外，帧内不应再出现 0x00
        for (std::size_t i = 0; i + 1 < enc.size(); ++i) CHECK(enc[i] != 0x00);
        std::vector<std::uint8_t> back;
        CHECK_OK(my::codec::cobs_decode(enc.data(), enc.size(), back));
        CHECK(back == all);
        // 去掉末尾分隔符同样能解（decode 带不带都收）
        std::vector<std::uint8_t> back_no_delim;
        CHECK_OK(my::codec::cobs_decode(enc.data(), enc.size() - 1, back_no_delim));
        CHECK(back_no_delim == all);
    }

    // 254 / 255 长块边界：码字 0xFF 处必须开新块
    for (std::size_t n : {std::size_t{253}, std::size_t{254}, std::size_t{255}, std::size_t{508},
                          std::size_t{509}}) {
        const std::vector<std::uint8_t> in(n, 0xAA);
        const auto enc = my::codec::cobs_encode(in.data(), in.size());
        CHECK(enc.size() <= my::codec::cobs_max_encoded_size(n));
        std::vector<std::uint8_t> back = {0x5A};
        CHECK_OK(my::codec::cobs_decode(enc.data(), enc.size(), back));
        CHECK(back == in);
    }

    // cobs_max_encoded_size 是上界：含 0x00 的数据、以及"最长"的 254 连续非零块都要满足
    for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{253},
                          std::size_t{254}, std::size_t{255}, std::size_t{1000}}) {
        const std::vector<std::uint8_t> zeros(n, 0x00);
        CHECK(my::codec::cobs_encode(zeros.data(), n).size() <= my::codec::cobs_max_encoded_size(n));
        std::vector<std::uint8_t> mixed;
        for (std::size_t i = 0; i < n; ++i) {
            mixed.push_back(static_cast<std::uint8_t>((i % 3 == 0) ? 0x00 : (i & 0xFF)));
        }
        CHECK(my::codec::cobs_encode(mixed.data(), n).size() <=
              my::codec::cobs_max_encoded_size(n));
    }

    // 非法帧：0x00 出现在中间 / 截断 / 空帧 -> ParseError，且 out 不变
    {
        std::vector<std::uint8_t> out = {0xAA};
        const std::vector<std::uint8_t> sentinel = out;
        const std::vector<std::vector<std::uint8_t>> bad = {
            {0x02, 0x41, 0x00, 0x42},  // 分隔符跑到帧中间
            {0x03, 0x11},              // 截断：码字 3 后面应有 2 字节
            {0x02},                    // 截断：码字 2 后面应有 1 字节
            {0x00},                    // 只有一个分隔符
            {},                        // 空帧
        };
        for (const auto& frame : bad) {
            const my::Status st = my::codec::cobs_decode(frame.data(), frame.size(), out);
            CHECK_EQ(st.code(), my::Errc::ParseError);
            CHECK(out == sentinel);
        }
    }
}

// -----------------------------------------------------------------------------
//  6. *_into 与 vector 版本逐字节一致；cap 少 1 -> OutOfRange
// -----------------------------------------------------------------------------
MYTEST_CASE(codec_cobs_into_matches_vector) {
    std::vector<std::vector<std::uint8_t>> inputs;
    inputs.push_back({});
    inputs.push_back({0x00});
    inputs.push_back({0x11, 0x22, 0x00, 0x33});
    inputs.push_back(all_bytes());
    inputs.push_back(std::vector<std::uint8_t>(254, 0xAA));
    inputs.push_back(std::vector<std::uint8_t>(255, 0x00));
    inputs.push_back(bytes("hello cobs"));

    for (const auto& in : inputs) {
        const std::vector<std::uint8_t> expect = my::codec::cobs_encode(in.data(), in.size());

        // 编码：cap 按 cobs_max_encoded_size 给足 -> 成功，内容与 vector 版一致
        std::vector<std::uint8_t> buf(my::codec::cobs_max_encoded_size(in.size()), 0xCC);
        const auto                enc =
            my::codec::cobs_encode_into(in.data(), in.size(), buf.data(), buf.size());
        CHECK_RESULT(enc);
        CHECK_EQ(enc.value(), expect.size());
        buf.resize(enc.value());
        CHECK(buf == expect);

        // cap 少 1 -> OutOfRange，且 dst 一个字节都不写
        std::vector<std::uint8_t> small(expect.size(), 0xCC);
        const auto                enc_small =
            my::codec::cobs_encode_into(in.data(), in.size(), small.data(), small.size() - 1);
        CHECK(!enc_small.ok());
        CHECK_EQ(enc_small.status().code(), my::Errc::OutOfRange);
        CHECK(untouched(small, 0xCC));

        // 解码：cap 给足 -> 成功且内容一致
        std::vector<std::uint8_t> decoded(in.size() + 1, 0xCC);
        const auto dec = my::codec::cobs_decode_into(expect.data(), expect.size(), decoded.data(),
                                                     decoded.size());
        CHECK_RESULT(dec);
        CHECK_EQ(dec.value(), in.size());
        decoded.resize(dec.value());
        CHECK(decoded == in);

        // 解码 cap 少 1 -> OutOfRange（空输入需要 0 字节，没有"少 1"可测）
        if (!in.empty()) {
            std::vector<std::uint8_t> dec_small(in.size(), 0xCC);
            const auto                dec_short = my::codec::cobs_decode_into(
                expect.data(), expect.size(), dec_small.data(), dec_small.size() - 1);
            CHECK(!dec_short.ok());
            CHECK_EQ(dec_short.status().code(), my::Errc::OutOfRange);
            CHECK(untouched(dec_small, 0xCC));
        }
    }

    // 参数错误：data==nullptr && size>0 / dst==nullptr && cap>0 -> InvalidArgument
    std::vector<std::uint8_t> buf(64, 0xCC);
    const auto                bad_data =
        my::codec::cobs_encode_into(nullptr, 3, buf.data(), buf.size());
    CHECK(!bad_data.ok());
    CHECK_EQ(bad_data.status().code(), my::Errc::InvalidArgument);

    const auto bad_dst = my::codec::cobs_encode_into(buf.data(), buf.size(), nullptr, 64);
    CHECK(!bad_dst.ok());
    CHECK_EQ(bad_dst.status().code(), my::Errc::InvalidArgument);

    const auto bad_dec_data = my::codec::cobs_decode_into(nullptr, 3, buf.data(), buf.size());
    CHECK(!bad_dec_data.ok());
    CHECK_EQ(bad_dec_data.status().code(), my::Errc::InvalidArgument);

    const auto bad_dec_dst = my::codec::cobs_decode_into(buf.data(), buf.size(), nullptr, 64);
    CHECK(!bad_dec_dst.ok());
    CHECK_EQ(bad_dec_dst.status().code(), my::Errc::InvalidArgument);
}
