#pragma once
// =============================================================================
//  my::log 的格式化便捷接口（引入 fmt）
//
//  与 log.hpp 分离的原因：log.hpp 保持零依赖，可放进对外公共头；
//  这个头引入 spdlog 自带的 fmt，编译期开销较大，按需包含。
//
//  my::log::info("x={} y={}", x, y);          // 仅当等级开启时才会格式化
// =============================================================================

#include <spdlog/fmt/fmt.h>

#include <utility>

#include "my_cpp_libs/log/log.hpp"

namespace my::log {

/// 直接格式化，返回 std::string
template <class... Args>
std::string format(fmt::format_string<Args...> fmt_str, Args&&... args) {
    // 必须写全 fmt::，否则 ADL 会把 fmt::format 一起引入导致重载歧义
    return fmt::format(fmt_str, std::forward<Args>(args)...);
}

#define MYLIB_LOG_DEFINE_FN(FN, LVL)                                                       \
    template <class... Args>                                                               \
    void FN(fmt::format_string<Args...> fmt_str, Args&&... args) {                          \
        if (should_log(Level::LVL)) {                                                      \
            write(Level::LVL,                                                              \
                  ::my::log::format(fmt_str, std::forward<Args>(args)...));                \
        }                                                                                  \
    }

MYLIB_LOG_DEFINE_FN(trace, Trace)
MYLIB_LOG_DEFINE_FN(debug, Debug)
MYLIB_LOG_DEFINE_FN(info, Info)
MYLIB_LOG_DEFINE_FN(warn, Warn)
MYLIB_LOG_DEFINE_FN(error, Error)
MYLIB_LOG_DEFINE_FN(critical, Critical)

#undef MYLIB_LOG_DEFINE_FN

}  // namespace my::log
