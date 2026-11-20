// =============================================================================
//  SHA-256 实现（FIPS 180-4）
//
//  结构：update() 只做缓冲与压缩，不分配；digest() 做尾部填充（0x80 + 0 填充 +
//  64 位大端比特长度），再压缩最后一两块。
//
//  正确性靠 tests/test_hash.cpp 里的标准测试向量守着：
//    ""          e3b0c442...
//    "abc"       ba7816bf...
//    两个块的串   248d6a61...
//    100 万个 'a' cdc76e5c...
//  这些位是公开已知的，抄错一位测试就会红 —— 这正是要测它们的原因。
// =============================================================================

#include "my_cpp_libs/core/hash.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

namespace my::hash {
namespace {

// FIPS 180-4 §4.2.2：前 64 个素数立方根小数部分的前 32 位
constexpr std::uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

inline std::uint32_t rotr(std::uint32_t x, int n) noexcept {
    return (x >> n) | (x << (32 - n));
}

inline std::uint32_t load_be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

inline void store_be32(std::uint32_t v, std::uint8_t* p) noexcept {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

constexpr char kHex[] = "0123456789abcdef";

char lower(char c) noexcept {
    return (c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

void Sha256::reset() noexcept {
    h_[0]     = 0x6a09e667u;
    h_[1]     = 0xbb67ae85u;
    h_[2]     = 0x3c6ef372u;
    h_[3]     = 0xa54ff53au;
    h_[4]     = 0x510e527fu;
    h_[5]     = 0x9b05688cu;
    h_[6]     = 0x1f83d9abu;
    h_[7]     = 0x5be0cd19u;
    total_    = 0;
    buf_len_  = 0;
}

void Sha256::compress(const std::uint8_t* block) noexcept {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = load_be32(block + i * 4);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i]                   = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    std::uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];

    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1  = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch  = (e & f) ^ (~e & g);
        const std::uint32_t t1  = h + s1 + ch + kK[i] + w[i];
        const std::uint32_t s0  = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2  = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(const void* data, std::size_t len) noexcept {
    if (len == 0) return;
    const auto* p = static_cast<const std::uint8_t*>(data);
    total_ += len;

    // 先把上次剩下的缓冲填满
    if (buf_len_ != 0) {
        const std::size_t take = (len < 64 - buf_len_) ? len : 64 - buf_len_;
        std::memcpy(buf_ + buf_len_, p, take);
        buf_len_ += take;
        p += take;
        len -= take;
        if (buf_len_ == 64) {
            compress(buf_);
            buf_len_ = 0;
        }
    }

    // 整块直接压缩，不进缓冲
    while (len >= 64) {
        compress(p);
        p += 64;
        len -= 64;
    }

    if (len != 0) {
        std::memcpy(buf_, p, len);
        buf_len_ = len;
    }
}

std::array<std::uint8_t, kSha256Bytes> Sha256::digest() noexcept {
    // 填充：0x80，然后补 0 到 56 (mod 64)，最后 8 字节是**比特**长度（大端）
    const std::uint64_t bits = total_ * 8u;

    const std::uint8_t one = 0x80;
    update(&one, 1);  // 注意：会顺带把 total_ 加 1，但下面用的是先算好的 bits

    const std::uint8_t zero = 0x00;
    while (buf_len_ != 56) {
        update(&zero, 1);
    }

    std::uint8_t len_be[8];
    for (int i = 0; i < 8; ++i) {
        len_be[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    }
    update(len_be, 8);

    std::array<std::uint8_t, kSha256Bytes> out{};
    for (int i = 0; i < 8; ++i) {
        store_be32(h_[i], out.data() + i * 4);
    }
    reset();
    return out;
}

std::string Sha256::hex_digest() {
    const auto d = digest();
    std::string s;
    s.resize(kSha256HexLen);
    for (std::size_t i = 0; i < kSha256Bytes; ++i) {
        s[i * 2]     = kHex[d[i] >> 4];
        s[i * 2 + 1] = kHex[d[i] & 0x0Fu];
    }
    return s;
}

std::string sha256_hex(const void* data, std::size_t len) {
    Sha256 h;
    h.update(data, len);
    return h.hex_digest();
}

std::string sha256_hex(std::string_view text) {
    return sha256_hex(text.data(), text.size());
}

Result<std::string> sha256_file_hex(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        const int err = errno;
        return Status(Errc::IoError,
                      "打开文件失败: " + path + " (" + std::strerror(err) +
                          ", errno=" + std::to_string(err) + ")");
    }

    Sha256 h;
    // 16 KiB 是"读得够快"与"板端栈不紧张"的折中：Zynq 的默认栈 8 MB，
    // 但库函数不该一口气吃掉 64 KiB 栈。
    std::uint8_t buf[16 * 1024];
    bool         read_error = false;

    for (;;) {
        const std::size_t n = std::fread(buf, 1, sizeof buf, f);
        if (n != 0) h.update(buf, n);
        if (n < sizeof buf) {
            read_error = (std::ferror(f) != 0);
            break;
        }
    }
    std::fclose(f);

    if (read_error) return Status(Errc::IoError, "读文件失败: " + path);
    return h.hex_digest();
}

bool digest_equal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    // 常数时间：不提前 return，差异累加到一个字节里
    unsigned diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned>(static_cast<unsigned char>(lower(a[i])) ^
                                      static_cast<unsigned char>(lower(b[i])));
    }
    return diff == 0;
}

}  // namespace my::hash
