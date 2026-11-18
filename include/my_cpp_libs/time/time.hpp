#pragma once
// =============================================================================
//  my::time —— 时钟、时间戳、计时
//
//  为什么需要它：这一层以前散在四处 ——
//    * log 自己拼时间戳；
//    * examples/bench 各自 steady_clock::now() 相减；
//    * 板端算耗时靠手写 chrono 表达式，单位（ms/us/s）在不同文件里不一致。
//
//  两条约定：
//    * **单调时钟用于测耗时**（monotonic_* / Stopwatch），系统时钟只用于"墙上时间"
//      （unix_ms / format_*）。Zynq 上 NTP 一跳，用系统时钟测出来的耗时可能是负的；
//    * 格式化默认 UTC（`...Z`），要本地时间显式调 format_local。
//      这样日志里不会因为时区/夏令时出现"同一个事件两个时间"。
// =============================================================================

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include "my_cpp_libs/export.hpp"

namespace my::time {

using SteadyClock = std::chrono::steady_clock;

// ----------------------------------------------------------------------------- 时钟
/// 单调时钟（不受系统时间调整影响），适合测耗时
MYLIB_API std::int64_t monotonic_us() noexcept;
MYLIB_API double       monotonic_s() noexcept;

/// 墙上时钟（Unix epoch 毫秒），适合时间戳/日志
MYLIB_API std::int64_t unix_ms() noexcept;

// ----------------------------------------------------------------------------- 格式化
/// "2026-11-18T10:54:09.123Z"（with_ms=false 时去掉毫秒）
MYLIB_API std::string format_utc(std::int64_t unix_ms, bool with_ms = true);

/// 本地时间，strftime 格式串；失败返回空串（不留半截字符串）
MYLIB_API std::string format_local(std::int64_t unix_ms, const char* fmt = "%Y-%m-%d %H:%M:%S");

/// 人类可读的耗时："0.42 ms" / "1.23 s" / "2m03s" / "1h02m"
MYLIB_API std::string format_elapsed(double seconds);

// ----------------------------------------------------------------------------- 计时
class Stopwatch {
public:
    Stopwatch() noexcept : start_(SteadyClock::now()) {}
    void   reset() noexcept { start_ = SteadyClock::now(); }
    double elapsed_s() const noexcept;
    double elapsed_ms() const noexcept { return elapsed_s() * 1000.0; }
    std::int64_t elapsed_us() const noexcept;

private:
    SteadyClock::time_point start_;
};

/// 作用域计时：析构时把耗时（秒）交给回调。回调为空则什么都不做。
/// 注意：析构里不抛异常；回调本身抛异常会被吞掉（日志/统计不该影响主流程）。
class ScopedTimer {
public:
    using Callback = std::function<void(double)>;

    explicit ScopedTimer(Callback on_done) : on_done_(std::move(on_done)) {}
    ~ScopedTimer();
    ScopedTimer(const ScopedTimer&)            = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

    double elapsed_s() const noexcept { return watch_.elapsed_s(); }
    /// 取消回调（比如中途发现这次不该计时）
    void cancel() noexcept { on_done_ = nullptr; }

private:
    Stopwatch watch_;
    Callback  on_done_;
};

// ----------------------------------------------------------------------------- 睡眠
MYLIB_API void sleep_ms(std::int64_t ms);
MYLIB_API void sleep_s(double seconds);

}  // namespace my::time
