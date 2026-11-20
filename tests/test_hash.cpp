// =============================================================================
//  sha256 的自测
//
//  分两类：
//    1. **公开已知的测试向量**（FIPS 180-4 / NIST）：这些位是标准的，抄错一位就红；
//    2. **不变式**：分段喂 vs 一次喂结果必须相同、字节边界（63/64/65）、
//       复用对象、文件摘要与内存摘要一致、常数时间比较。
//
//  为什么不"自己算一个期望值再比"：那样等于用同一个可能有 bug 的实现验证自己。
// =============================================================================

#include "my_cpp_libs/core/hash.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "test_framework.hpp"

MYTEST_SUITE("hash");

namespace {

/// "abc" 重复 n 次（用来造 100 万字节的测试串，避免源码里放一兆字符）
std::string repeat(const std::string& unit, std::size_t total) {
    std::string s;
    s.reserve(total);
    while (s.size() < total) s += unit;
    s.resize(total);
    return s;
}

std::string tmp_path(const char* name) {
#if defined(_WIN32)
    const char* dir = std::getenv("TEMP");
    if (dir == nullptr) dir = ".";
    return std::string(dir) + "\\mylib_hash_" + name;
#else
    const char* dir = std::getenv("TMPDIR");
    if (dir == nullptr) dir = "/tmp";
    return std::string(dir) + "/mylib_hash_" + name;
#endif
}

}  // namespace

// =============================================================================
//  一、标准测试向量
// =============================================================================
MYTEST_CASE(hash_sha256_known_vectors) {
    // 空串
    CHECK_EQ(my::hash::sha256_hex("", 0),
             std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));

    // "abc"（NIST 最经典的一条）
    CHECK_EQ(my::hash::sha256_hex(std::string_view("abc")),
             std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    // 448 位（两个 block 的经典向量），正好卡在填充边界
    const std::string two_block =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK_EQ(my::hash::sha256_hex(std::string_view(two_block)),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

    // 100 万个 'a'
    const std::string million = repeat("a", 1000000);
    CHECK_EQ(my::hash::sha256_hex(std::string_view(million)),
             std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

MYTEST_CASE(hash_sha256_hex_is_lowercase_64) {
    const auto h = my::hash::sha256_hex(std::string_view("x"));
    CHECK_EQ(h.size(), std::size_t{my::hash::kSha256HexLen});
    for (char c : h) {
        EXPECT((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
}

// =============================================================================
//  二、不变式
// =============================================================================
MYTEST_CASE(hash_sha256_streaming_equals_oneshot) {
    // 造一份带所有字节值的数据，覆盖各种对齐
    std::vector<std::uint8_t> data(300);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>(i * 37 + 11);
    }

    for (std::size_t len : {std::size_t{0}, std::size_t{1}, std::size_t{55}, std::size_t{56},
                            std::size_t{63}, std::size_t{64}, std::size_t{65}, std::size_t{127},
                            std::size_t{128}, std::size_t{129}, std::size_t{255}}) {
        const auto one_shot = my::hash::sha256_hex(data.data(), len);

        my::hash::Sha256 h;
        h.update(data.data(), len);
        CHECK_EQ(h.hex_digest(), one_shot);

        // 逐字节喂
        my::hash::Sha256 b;
        for (std::size_t i = 0; i < len; ++i) b.update(&data[i], 1);
        CHECK_EQ(b.hex_digest(), one_shot);

        // 三分段喂（故意切在不对齐的位置）
        my::hash::Sha256 c;
        const std::size_t   p1 = len / 3;
        const std::size_t   p2 = len / 3 * 2;
        c.update(data.data(), p1);
        c.update(data.data() + p1, p2 - p1);
        c.update(data.data() + p2, len - p2);
        CHECK_EQ(c.hex_digest(), one_shot);
    }
}

MYTEST_CASE(hash_sha256_update_zero_len_is_noop) {
    my::hash::Sha256 h;
    const char      x = 'x';
    h.update(&x, 1);
    const auto a = h.hex_digest();

    my::hash::Sha256 g;
    g.update(nullptr, 0);  // 允许：长度为 0 时指针可以为空
    g.update(&x, 1);
    g.update(&x, 0);
    CHECK_EQ(g.hex_digest(), a);
}

MYTEST_CASE(hash_sha256_digest_resets_for_reuse) {
    my::hash::Sha256 h;
    h.update("abc", 3);
    const auto first = h.hex_digest();
    CHECK_EQ(first, my::hash::sha256_hex(std::string_view("abc")));

    // 取完摘要对象已经复位：再喂一次同样的数据必须得到同样的结果
    h.update("abc", 3);
    CHECK_EQ(h.hex_digest(), first);

    // digest() 给 32 字节，与 hex 一致
    h.update("abc", 3);
    const auto raw = h.digest();
    CHECK_EQ(raw.size(), std::size_t{my::hash::kSha256Bytes});
    const std::string expect = my::hash::sha256_hex(std::string_view("abc"));
    for (std::size_t i = 0; i < raw.size(); ++i) {
        char hex[3] = {};
        std::snprintf(hex, sizeof hex, "%02x", static_cast<unsigned>(raw[i]));
        EXPECT_EQ(std::string(hex), expect.substr(i * 2, 2));
    }
}

MYTEST_CASE(hash_sha256_file_matches_memory) {
    const std::string path = tmp_path("vector.bin");
    const std::string body = "the quick brown fox jumps over the lazy dog";

    std::FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    if (f == nullptr) return;
    std::fwrite(body.data(), 1, body.size(), f);
    std::fclose(f);

    auto file_hash = my::hash::sha256_file_hex(path);
    CHECK_OK_VALUE(fh, file_hash);
    CHECK_EQ(fh, my::hash::sha256_hex(std::string_view(body)));

    // 空文件也要能算（容易写成 fread==0 就报错）
    const std::string empty_path = tmp_path("empty.bin");
    std::FILE*        fe         = std::fopen(empty_path.c_str(), "wb");
    CHECK(fe != nullptr);
    if (fe) std::fclose(fe);
    auto eh = my::hash::sha256_file_hex(empty_path);
    CHECK_OK_VALUE(ehv, eh);
    CHECK_EQ(ehv,
             std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));

    std::remove(path.c_str());
    std::remove(empty_path.c_str());
}

MYTEST_CASE(hash_sha256_file_missing_returns_error) {
    auto r = my::hash::sha256_file_hex(tmp_path("definitely-not-here-xyz"));
    CHECK(!r.ok());
    CHECK_EQ(r.status().code(), my::Errc::IoError);
    // 消息里要带路径，便于排查
    CHECK(r.status().message().find("definitely-not-here-xyz") != std::string::npos);
}

MYTEST_CASE(hash_digest_equal_semantics) {
    const char* a = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    const char* b = "ba7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD";

    CHECK(my::hash::digest_equal(a, a));
    CHECK(my::hash::digest_equal(a, b));  // 大小写不敏感
    CHECK(my::hash::digest_equal("", ""));
    CHECK(!my::hash::digest_equal(a, ""));  // 长度不同
    CHECK(my::hash::digest_equal(std::string_view(a, 63), std::string_view(a, 63)));

    // 只差最后一位
    std::string almost(a, 64);
    almost[63]       = (almost[63] == 'd') ? 'e' : 'd';
    CHECK(!my::hash::digest_equal(a, almost));
}
