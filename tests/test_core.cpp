#include "test_framework.hpp"

#include <my_cpp_libs/core/build_info.hpp>
#include <my_cpp_libs/core/half.hpp>
#include <my_cpp_libs/core/status.hpp>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

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