#include "my_cpp_libs/time/time.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>
#include <utility>

namespace my::time {
namespace {

/// 跨平台的 gmtime/localtime 线程安全版本
bool gmtime_safe(std::time_t t, std::tm& out) noexcept {
#if defined(_WIN32)
    return ::gmtime_s(&out, &t) == 0;
#else
    return ::gmtime_r(&t, &out) != nullptr;
#endif
}

bool localtime_safe(std::time_t t, std::tm& out) noexcept {
#if defined(_WIN32)
    return ::localtime_s(&out, &t) == 0;
#else
    return ::localtime_r(&t, &out) != nullptr;
#endif
}

}  // namespace

// ----------------------------------------------------------------------------- 时钟
std::int64_t monotonic_us() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now().time_since_epoch())
        .count();
}

double monotonic_s() noexcept {
    return static_cast<double>(monotonic_us()) / 1e6;
}

std::int64_t unix_ms() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

// ----------------------------------------------------------------------------- 格式化
std::string format_utc(std::int64_t ms, bool with_ms) {
    const auto seconds = static_cast<std::int64_t>(ms / 1000);
    const auto millis  = static_cast<int>(ms % 1000);

    std::tm tmv{};
    const auto secs = static_cast<std::time_t>(seconds);
    if (!gmtime_safe(secs, tmv)) return {};

    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv) == 0) return {};

    std::string out(buf);
    if (with_ms) {
        char frac[8];
        // ms 可能为负（1970 之前），取绝对值避免 "-0.xxx"
        std::snprintf(frac, sizeof(frac), ".%03d", millis < 0 ? -millis : millis);
        out += frac;
    }
    out += "Z";
    return out;
}

std::string format_local(std::int64_t ms, const char* fmt) {
    if (fmt == nullptr) return {};
    const auto seconds = static_cast<std::int64_t>(ms / 1000);

    std::tm tmv{};
    const auto secs = static_cast<std::time_t>(seconds);
    if (!localtime_safe(secs, tmv)) return {};

    char buf[128];
    if (std::strftime(buf, sizeof(buf), fmt, &tmv) == 0) return {};
    return std::string(buf);
}

std::string format_elapsed(double seconds) {
    if (!std::isfinite(seconds)) return "n/a";
    if (seconds < 0.0) return "n/a";

    char buf[48];
    if (seconds < 1e-3) {
        std::snprintf(buf, sizeof(buf), "%.1f us", seconds * 1e6);
    } else if (seconds < 1.0) {
        std::snprintf(buf, sizeof(buf), "%.2f ms", seconds * 1e3);
    } else if (seconds < 60.0) {
        std::snprintf(buf, sizeof(buf), "%.3f s", seconds);
    } else if (seconds < 3600.0) {
        const int m = static_cast<int>(seconds) / 60;
        const int s = static_cast<int>(seconds) % 60;
        std::snprintf(buf, sizeof(buf), "%dm%02ds", m, s);
    } else {
        const int h = static_cast<int>(seconds) / 3600;
        const int m = (static_cast<int>(seconds) % 3600) / 60;
        std::snprintf(buf, sizeof(buf), "%dh%02dm", h, m);
    }
    return buf;
}

// ----------------------------------------------------------------------------- 计时
double Stopwatch::elapsed_s() const noexcept {
    const auto d = SteadyClock::now() - start_;
    return std::chrono::duration<double>(d).count();
}

std::int64_t Stopwatch::elapsed_us() const noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - start_).count();
}

ScopedTimer::~ScopedTimer() {
    if (!on_done_) return;
    const double s = watch_.elapsed_s();
    // 回调抛异常不能从析构里逃出去（那会直接 terminate）。
    // time 组件在 -fno-exceptions 子集里：那种构建下 try/catch 根本编不过，
    // 所以按"异常是否开启"裁剪 —— 关异常时调用方自己负责别让回调抛。
#if defined(__cpp_exceptions) || defined(_CPPUNWIND) || defined(__EXCEPTIONS)
    try {
        on_done_(s);
    } catch (...) {
    }
#else
    on_done_(s);
#endif
}

// ----------------------------------------------------------------------------- 睡眠
void sleep_ms(std::int64_t ms) {
    if (ms <= 0) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void sleep_s(double seconds) {
    if (!(seconds > 0.0)) return;
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

}  // namespace my::time
