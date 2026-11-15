#include "test_framework.hpp"

#include <my_cpp_libs/core/build_info.hpp>
#include <my_cpp_libs/core/half.hpp>
#include <my_cpp_libs/core/status.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

MYTEST_SUITE("core");

MYTEST_CASE(status_basic) {
    my::Status ok;
    CHECK(ok.ok());
    CHECK(static_cast<bool>(ok));
    CHECK_EQ(std::string(ok.to_string()), std::string("ok"));

    my::Status err(my::Errc::IoError, "boom");
    CHECK(!err.ok());
    CHECK(err.failed());
    CHECK_EQ(err.code(), my::Errc::IoError);
    CHECK_EQ(err.message(), std::string("boom"));
    CHECK_EQ(err.to_string(), std::string("io_error: boom"));
}

MYTEST_CASE(errc_names) {
    CHECK_EQ(std::string(my::errc_name(my::Errc::Ok)), std::string("ok"));
    CHECK_EQ(std::string(my::errc_name(my::Errc::InvalidArgument)), std::string("invalid_argument"));
    CHECK_EQ(std::string(my::errc_name(my::Errc::ParseError)), std::string("parse_error"));
    CHECK_EQ(std::string(my::errc_name(my::Errc::Unsupported)), std::string("unsupported"));
}

MYTEST_CASE(result_value) {
    my::Result<int> r = 42;
    CHECK(r.ok());
    CHECK_EQ(r.value(), 42);
    CHECK_EQ(*r, 42);
    CHECK_EQ(*r.operator->(), 42);
}

MYTEST_CASE(result_error) {
    my::Result<int> r(my::Status(my::Errc::NotFound, "missing"));
    CHECK(!r.ok());
    CHECK_EQ(r.status().code(), my::Errc::NotFound);
    CHECK_EQ(r.value_or(7), 7);

    auto doubled = r.map([](int v) { return v * 2; });
    CHECK(!doubled.ok());
    CHECK_EQ(doubled.status().code(), my::Errc::NotFound);
}

MYTEST_CASE(result_move_only) {
    my::Result<std::vector<int>> r = std::vector<int>{1, 2, 3};
    CHECK_OK(r.status());
    CHECK_EQ(r.value().size(), std::size_t{3});

    my::Result<std::unique_ptr<int>> u(std::unique_ptr<int>(new int(5)));
    CHECK(u.ok());
    CHECK_EQ(*u.value(), 5);
}

MYTEST_CASE(result_implicit_convert) {
    my::Result<double> d = 1.0f;  // float -> double
    CHECK(d.ok());
    CHECK_NEAR(d.value(), 1.0, 1e-9);

    my::Result<std::string> s = "hello";  // const char* -> std::string
    CHECK(s.ok());
    CHECK_EQ(s.value(), std::string("hello"));
}

MYTEST_CASE(build_info) {    CHECK(std::strlen(my::BuildInfo::libraryVersion()) > 0);
    CHECK(std::strlen(my::BuildInfo::compiler()) > 0);
    CHECK(std::strlen(my::BuildInfo::targetTriple()) > 0);
    CHECK(std::strlen(my::BuildInfo::summary()) > 0);
    const int bits = my::BuildInfo::pointerBits();
    CHECK(bits == 32 || bits == 64);
    // 64 位目标指针位宽必须是 64
    CHECK_EQ(bits, static_cast<int>(sizeof(void*) * 8));

    const std::string os = my::BuildInfo::osName();
    CHECK(os == "windows" || os == "linux" || os == "macos");
}

// -----------------------------------------------------------------------------
//  fp16 <-> fp32
// -----------------------------------------------------------------------------
MYTEST_CASE(half_roundtrip_common) {
    const float samples[] = {0.0f, 1.0f, -1.0f, 0.5f, 0.1f, 2.5f, -3.75f, 100.0f};
    for (float v : samples) {
        const float back = my::half_to_float(my::float_to_half(v));
        CHECK_NEAR(back, v, 0.002);  // fp16 只有约 3 位十进制精度
    }
}

MYTEST_CASE(half_special_values) {
    // 0x3C00 = 1.0, 0x4000 = 2.0, 0xC000 = -2.0, 0x7C00 = +inf
    CHECK_NEAR(my::half_to_float(0x3C00), 1.0, 1e-9);
    CHECK_NEAR(my::half_to_float(0x4000), 2.0, 1e-9);
    CHECK_NEAR(my::half_to_float(0xC000), -2.0, 1e-9);
    CHECK_NEAR(my::half_to_float(0x0000), 0.0, 1e-9);

    const float inf = my::half_to_float(0x7C00);
    CHECK(std::isinf(inf) && inf > 0);
    CHECK(std::isnan(my::half_to_float(0x7E00)));

    // 溢出 -> inf
    CHECK(std::isinf(my::half_to_float(my::float_to_half(1.0e30f))));

    // 极小值 -> 0 或次正规数
    const float tiny = my::half_to_float(my::float_to_half(1.0e-9f));
    CHECK_NEAR(tiny, 0.0, 1e-9);

    // 次正规数可表示的最小正值 0x0001 -> 约 5.96e-8
    CHECK_NEAR(my::half_to_float(0x0001), 5.960464477539063e-08, 1e-14);
}

MYTEST_CASE(half_negative_and_rounding) {
    CHECK_NEAR(my::half_to_float(my::float_to_half(-0.333f)), -0.333, 0.001);
    // 65504 是 fp16 最大有限值
    CHECK_NEAR(my::half_to_float(my::float_to_half(65504.0f)), 65504.0, 1.0);
}

// 位精确校验：这些值以前要么踩到移位未定义行为，要么舍入方向错 1 ULP
MYTEST_CASE(half_bit_exact) {
    constexpr std::uint16_t kZero      = 0x0000u;
    constexpr std::uint16_t kMinSub    = 0x0001u;  // 2^-24 = 5.96e-8
    constexpr std::uint16_t kNegZero   = 0x8000u;
    constexpr std::uint16_t kOne       = 0x3C00u;
    constexpr std::uint16_t kOneUp     = 0x3C01u;
    constexpr std::uint16_t kMaxFinite = 0x7BFFu;  // 65504
    constexpr std::uint16_t kInf       = 0x7C00u;

    // 符号与零
    CHECK_EQ(my::float_to_half(0.0f), kZero);
    CHECK_EQ(my::float_to_half(-0.0f), kNegZero);

    // 次正规边界：0x33000000 恰好是 2^-25，是 0 与 2^-24 的中点 -> 向偶数舍入到 0
    const float half_ulp = std::ldexp(1.0f, -25);
    CHECK_EQ(my::float_to_half(half_ulp), kZero);
    CHECK_EQ(my::float_to_half(std::nextafter(half_ulp, 1.0f)), kMinSub);
    CHECK_EQ(my::float_to_half(std::ldexp(1.0f, -24)), kMinSub);

    // 最大的次正规数（0x03FF）与最小的正规数（0x0400）之间不能跳错
    CHECK_EQ(my::float_to_half(1023.0f * std::ldexp(1.0f, -24)), static_cast<std::uint16_t>(0x03FF));
    CHECK_EQ(my::float_to_half(std::ldexp(1.0f, -14)), static_cast<std::uint16_t>(0x0400));

    // 次正规区向上舍入会进位到最小正规数 2^-14：
    // 掩码写成 & 0x3FF 会把 k=1024 变成 0（这条曾经真的错过）
    const float mid_sub_to_norm = 1023.5f * std::ldexp(1.0f, -24);  // 恰好是中点
    CHECK_EQ(my::float_to_half(mid_sub_to_norm), static_cast<std::uint16_t>(0x0400));
    CHECK_EQ(my::float_to_half(std::ldexp(1.0f, -14) * (1.0f - 1.0f / 4096.0f)),
             static_cast<std::uint16_t>(0x0400));
    CHECK_EQ(my::float_to_half(std::nextafter(mid_sub_to_norm, 0.0f)),
             static_cast<std::uint16_t>(0x03FF));

    // 原来的未定义行为区间：|v| 远小于 2^-25 时必须安静地给出 0，而不是垃圾值
    CHECK_EQ(my::float_to_half(7.3e-12f), kZero);
    CHECK_EQ(my::float_to_half(-7.3e-12f), kNegZero);
    CHECK_EQ(my::float_to_half(std::numeric_limits<float>::denorm_min()), kZero);

    // 舍入必须"向偶数"：1.0 与 1.0009765625 的中点回到 1.0（偶数尾数）
    const float tie = 1.0f + std::ldexp(1.0f, -11);
    CHECK_EQ(my::float_to_half(tie), kOne);
    CHECK_EQ(my::float_to_half(std::nextafter(tie, 2.0f)), kOneUp);
    // 2.0 的相邻值跨度是 2^-10，中点同样向偶数舍入
    const float tie2 = 2.0f + std::ldexp(1.0f, -10);
    CHECK_EQ(my::float_to_half(tie2), static_cast<std::uint16_t>(0x4000));

    // 上溢边界：65504 保留，65520（= 65504 + 16）是到 inf 的中点 -> 向偶数舍入成 inf
    CHECK_EQ(my::float_to_half(65504.0f), kMaxFinite);
    CHECK_EQ(my::float_to_half(65519.0f), kMaxFinite);
    CHECK_EQ(my::float_to_half(65520.0f), kInf);
    CHECK_EQ(my::float_to_half(-70000.0f), static_cast<std::uint16_t>(0xFC00));

    // NaN 必须还是 NaN（指数全 1 且尾数非 0）
    const std::uint16_t nan_bits = my::float_to_half(std::numeric_limits<float>::quiet_NaN());
    CHECK_EQ(static_cast<std::uint16_t>(nan_bits & 0x7C00u), kInf);
    CHECK((nan_bits & 0x03FFu) != 0u);
    CHECK(std::isnan(my::half_to_float(nan_bits)));

    // 次正规数往返：0x0001 / 0x03FF 必须原样回来
    CHECK_EQ(my::float_to_half(my::half_to_float(0x0001u)), static_cast<std::uint16_t>(0x0001u));
    CHECK_EQ(my::float_to_half(my::half_to_float(0x03FFu)), static_cast<std::uint16_t>(0x03FFu));
    // 中位数附近的往返也不能跳两格
    for (std::uint32_t bits = 0x0000u; bits <= 0x7BFFu; ++bits) {
        const std::uint16_t back = my::float_to_half(my::half_to_float(static_cast<std::uint16_t>(bits)));
        CHECK_EQ(back, static_cast<std::uint16_t>(bits));
    }
}