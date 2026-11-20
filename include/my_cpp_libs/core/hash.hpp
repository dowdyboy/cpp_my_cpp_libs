#pragma once
// =============================================================================
//  my::hash —— SHA-256（FIPS 180-4），无三方依赖
//
//  为什么自己写：板端要校验"这个 .onnx / 固件 / 数据文件是不是我以为的那个"，
//  需要的只是一个哈希函数。引 OpenSSL 等于为 200 行代码拖进一个几 MB 的库，
//  而且交叉编译多一层坑；RFC 6234 / FIPS 180-4 的实现短到可以当看家本领。
//
//  用途：
//    * `my::hash::sha256_file_hex()` 校验模型/固件（本项目里用于 ml 的 sidecar 校验）；
//    * `digest_equal()` 做**常数时间**比较 —— 不要用 `==` 比较摘要，
//      时序侧信道在"比较来自外部的值"时是真实的攻击面。
//
//  约定：
//    * 全程不抛异常（`update()` 不分配；`hex_digest()` 分配返回值是唯一可能的分配）；
//    * `digest()` / `hex_digest()` 会**结束**当前计算并把对象复位，
//      想复用对象继续算下一段数据是可以的，但要先把摘要取走。
// =============================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "my_cpp_libs/core/status.hpp"
#include "my_cpp_libs/export.hpp"

namespace my::hash {

/// 摘要长度（字节）与十六进制表示长度
inline constexpr std::size_t kSha256Bytes = 32;
inline constexpr std::size_t kSha256HexLen = 64;

/// 增量式 SHA-256。对同一份数据，"分成几段 update" 与 "一次 update" 结果必须相同。
class MYLIB_API Sha256 {
public:
    Sha256() noexcept { reset(); }

    /// 喂数据。允许 len == 0 与 data == nullptr（当 len == 0 时）。
    void update(const void* data, std::size_t len) noexcept;

    /// 取原始摘要（32 字节），并把对象复位。
    std::array<std::uint8_t, kSha256Bytes> digest() noexcept;

    /// 取小写十六进制摘要（64 字符），并把对象复位。
    std::string hex_digest();

    /// 回到初始状态。
    void reset() noexcept;

private:
    void compress(const std::uint8_t* block) noexcept;

    std::uint32_t h_[8]{};
    std::uint64_t total_ = 0;  ///< 已喂入的**字节**数（内部按位换算）
    std::uint8_t  buf_[64]{};
    std::size_t   buf_len_ = 0;
};

/// 一次性：任意数据 → 小写十六进制摘要（64 字符）。
MYLIB_API std::string sha256_hex(const void* data, std::size_t len);
MYLIB_API std::string sha256_hex(std::string_view text);

/// 文件摘要：16 KiB 分块读，不会把整个文件读进内存。
/// 文件打不开 / 读失败返回 Status（消息里带路径与 errno）。
MYLIB_API Result<std::string> sha256_file_hex(const std::string& path);

/// 常数时间比较两个十六进制摘要（大小写不敏感）。
/// 长度不同直接 false —— 长度本身不是秘密（摘要定长 64）。
MYLIB_API bool digest_equal(std::string_view a, std::string_view b) noexcept;

}  // namespace my::hash
