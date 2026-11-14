// =============================================================================
//  示例 10：spdlog 原生 API（不经过任何封装）
//
//  用途：对照学习 spdlog 本身的常用写法。项目里统一用 my::log 时，
//        这份代码可以当作"my::log 底层做了什么"的参考。
// =============================================================================

#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <memory>
#include <vector>

int main() {
    // -------------------------------------------------------------------------
    //  1) 最快的用法：默认 logger 直接打
    // -------------------------------------------------------------------------
    spdlog::info("Hello, {}! 这是默认 logger", "spdlog");
    spdlog::warn("默认 logger 输出到 stdout");
    spdlog::set_level(spdlog::level::debug);  // 设置全局级别
    spdlog::debug("debug 已打开");
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");  // 改格式

    // -------------------------------------------------------------------------
    //  2) 多 sink 组合：控制台 + 滚动文件
    // -------------------------------------------------------------------------
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::info);

    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        "output/spdlog_native.log", 1024 * 1024, 3);
    file_sink->set_level(spdlog::level::debug);

    std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};
    auto multi = std::make_shared<spdlog::logger>("multi", sinks.begin(), sinks.end());
    spdlog::register_logger(multi);

    multi->set_level(spdlog::level::debug);
    multi->flush_on(spdlog::level::warn);  // WARN 及以上立即刷盘

    multi->info("同一个 logger 同时写控制台和文件");
    multi->error("错误也会立刻落盘");

    // -------------------------------------------------------------------------
    //  3) 异步 logger：高吞吐场景
    // -------------------------------------------------------------------------
    spdlog::init_thread_pool(8192, 1);
    auto async = std::make_shared<spdlog::async_logger>(
        "async", sinks.begin(), sinks.end(), spdlog::thread_pool(),
        spdlog::async_overflow_policy::block);
    async->set_level(spdlog::level::info);

    for (int i = 0; i < 5; ++i) async->info("异步日志 #{}", i);

    // -------------------------------------------------------------------------
    //  4) 格式化细节
    // -------------------------------------------------------------------------
    multi->info("整数={:05d} 浮点={:.3f} 十六进制={:#x} 字符串={:>8}", 42, 3.14159, 255, "right");
    multi->info("转义花括号 {{}} 与常量 {}", "value");

    // -------------------------------------------------------------------------
    //  5) 收尾
    // -------------------------------------------------------------------------
    spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& lg) { lg->flush(); });
    spdlog::shutdown();  // 异步 logger 必须 shutdown 才能保证退出前写完

    std::printf("\n原生 spdlog 示例完成，文件见 output/spdlog_native.log\n");
    return 0;
}
