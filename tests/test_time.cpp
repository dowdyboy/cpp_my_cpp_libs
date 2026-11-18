// =============================================================================
//  time 组件：单调时钟 / 时间戳格式化 / 计时器
#include "my_cpp_libs/time/time.hpp"

#include <cstdio>
#include <string>

#include "my_cpp_libs/core/build_info.hpp"  // BuildInfo::hasExceptions()
#include "test_framework.hpp"

MYTEST_SUITE("time");

MYTEST_CASE(time_monotonic_is_non_decreasing) {
    const auto a = my::time::monotonic_us();
    const auto b = my::time::monotonic_us();
    CHECK(a > 0);
    CHECK(b >= a);

    const double s = my::time::monotonic_s();
    CHECK(s > 0.0);
    CHECK(s < 1e12);  // 秒量级不该是天文数字（us/ms 混用会立刻超界）
}

MYTEST_CASE(time_sleep_advances_monotonic) {
    const auto  before = my::time::monotonic_us();
    my::time::sleep_ms(20);
    const auto after = my::time::monotonic_us();

    const std::int64_t delta_us = after - before;
    CHECK(delta_us >= 15000);    // 至少睡了 15ms（留调度余量）
    CHECK(delta_us < 2000000);   // 也不该睡出 2 秒
}

MYTEST_CASE(time_unix_ms_is_reasonable) {
    const auto ms = my::time::unix_ms();
    // 2020-01-01 之后、2100 之前
    CHECK(ms > 1577836800000LL);
    CHECK(ms < 4102444800000LL);
}

MYTEST_CASE(time_format_utc_epoch) {
    CHECK_EQ(my::time::format_utc(0), std::string("1970-01-01T00:00:00.000Z"));
    CHECK_EQ(my::time::format_utc(0, /*with_ms=*/false), std::string("1970-01-01T00:00:00Z"));
}

MYTEST_CASE(time_format_utc_known_moment) {
    // 1700000000 s = 2023-11-14T22:13:20Z
    CHECK_EQ(my::time::format_utc(1700000000000LL), std::string("2023-11-14T22:13:20.000Z"));
    CHECK_EQ(my::time::format_utc(1700000000123LL), std::string("2023-11-14T22:13:20.123Z"));
    // 毫秒位必须补零到 3 位
    CHECK_EQ(my::time::format_utc(1700000000007LL), std::string("2023-11-14T22:13:20.007Z"));
}

MYTEST_CASE(time_format_local_is_wellformed) {
    const std::string local = my::time::format_local(1700000000000LL);
    CHECK(!local.empty());
    CHECK(local.find(':') != std::string::npos);   // 至少是个时间串
    CHECK_EQ(local.size(), std::size_t{19});       // "YYYY-MM-DD HH:MM:SS"

    // 非法格式串不能返回半截字符串
    CHECK(my::time::format_local(1700000000000LL, nullptr).empty());
}

MYTEST_CASE(time_format_elapsed_units) {
    CHECK_EQ(my::time::format_elapsed(0.0005), std::string("500.0 us"));
    CHECK_EQ(my::time::format_elapsed(0.5), std::string("500.00 ms"));
    CHECK_EQ(my::time::format_elapsed(2.5), std::string("2.500 s"));
    CHECK_EQ(my::time::format_elapsed(90.0), std::string("1m30s"));
    CHECK_EQ(my::time::format_elapsed(3700.0), std::string("1h01m"));
    // 负数/NaN 不该输出垃圾
    CHECK_EQ(my::time::format_elapsed(-1.0), std::string("n/a"));
}

MYTEST_CASE(time_stopwatch_measures_sleep) {
    my::time::Stopwatch watch;
    my::time::sleep_ms(15);

    const double ms = watch.elapsed_ms();
    CHECK(ms >= 10.0);
    CHECK(ms < 1000.0);
    CHECK(watch.elapsed_us() >= 10000);

    watch.reset();
    CHECK(watch.elapsed_ms() < 10.0);  // reset 之后要回到接近 0
}

MYTEST_CASE(time_scoped_timer_calls_back) {
    double captured = -1.0;
    {
        my::time::ScopedTimer timer([&](double s) { captured = s; });
        my::time::sleep_ms(10);
    }
    CHECK(captured >= 0.0);
    CHECK(captured < 1.0);
}

MYTEST_CASE(time_scoped_timer_can_be_cancelled) {
    bool called = false;
    {
        my::time::ScopedTimer timer([&](double) { called = true; });
        timer.cancel();
    }
    CHECK(!called);
}

MYTEST_CASE(time_scoped_timer_swallows_callback_exception) {
    // 回调抛异常不能从析构里逃出去（那会直接 terminate）。
    // 但这只有在**库本身开了异常**时才成立：-fno-exceptions 构建里
    // ScopedTimer 的析构里没有 try/catch，回调抛出去就是 terminate。
    if (!my::BuildInfo::hasExceptions()) {
        MYTEST_SKIP("-fno-exceptions 构建：析构里没有 try/catch，回调不允许抛");
    }
    {
        my::time::ScopedTimer timer([](double) { throw 42; });
        my::time::sleep_ms(1);
    }
    CHECK(true);  // 能走到这里就说明没 terminate
}
