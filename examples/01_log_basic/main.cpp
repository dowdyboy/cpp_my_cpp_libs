// =============================================================================
//  示例 01：日志
//  演示 my::log 的常用能力：分级、异步、文件滚动、自定义 sink。
//  对照原生 spdlog 写法见 examples/10_native_spdlog。
// =============================================================================

#include <my_cpp_libs/core/build_info.hpp>
#include <my_cpp_libs/log/format.hpp>
#include <my_cpp_libs/log/log.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

int main() {
    std::printf("=== my_cpp_libs 日志示例 ===\n");
    std::printf("构建信息: %s\n\n", my::BuildInfo::summary());

    // 1) 初始化：控制台 + 滚动文件，异步
    std::error_code ec;
    std::filesystem::create_directories("output", ec);

    my::log::Config config;
    config.logger_name = "demo";
    config.level       = my::log::Level::Debug;
    config.to_console  = true;
    config.use_color   = true;
    config.file_path   = "output/demo.log";
    config.rotate_bytes = 1u * 1024u * 1024u;  // 1MB
    config.rotate_files = 2;
    config.async        = true;
    config.async_queue_size = 4096;

    auto status = my::log::init(config);
    if (!status) {
        std::printf("日志初始化失败: %s\n", status.to_string().c_str());
        return 1;
    }

    // 2) 五个等级
    my::log::trace("trace 一般不打开");
    my::log::debug("debug: 变量 x={}, 名字={}", 42, "spdlog");
    my::log::info("info: 启动完成，耗时 {} ms", 12.5);
    my::log::warn("warn: 队列长度 {} 超过阈值", 8000);
    my::log::error("error: 设备 {} 无响应", 3);
    my::log::critical("critical: 即将退出");

    // 3) 运行期调级别
    my::log::set_level(my::log::Level::Warn);
    my::log::info("这条不会输出（级别已提到 Warn）");
    my::log::warn("这条会输出");
    my::log::set_level(my::log::Level::Info);

    // 4) 避免昂贵参数构造：先判断
    if (my::log::should_log(my::log::Level::Debug)) {
        my::log::debug("只有 Debug 打开时才会构造: {}", std::string(64, 'x'));
    }

    // 5) 自定义 sink —— 把日志同时导到别处（这里演示统计个级别条数）
    int counters[4] = {0, 0, 0, 0};
    my::log::set_sink([&counters](my::log::Level level, std::string_view) {
        switch (level) {
            case my::log::Level::Warn: ++counters[0]; break;
            case my::log::Level::Error: ++counters[1]; break;
            case my::log::Level::Critical: ++counters[2]; break;
            default: ++counters[3]; break;
        }
    });
    my::log::warn("自定义 sink 捕获 1");
    my::log::error("自定义 sink 捕获 2");
    my::log::info("自定义 sink 捕获 3");
    my::log::clear_sink();

    std::printf("\n自定义 sink 统计: warn=%d error=%d critical=%d 其它=%d\n",
                counters[0], counters[1], counters[2], counters[3]);

    // 6) 异步日志必须 flush/shutdown 才能保证落盘
    my::log::flush();
    my::log::shutdown();

    // 7) 未初始化时也不会丢日志（落到 stderr）
    my::log::info("shutdown 之后这条走 stderr 兜底");

    std::printf("\n日志文件: %s\n",
                std::filesystem::absolute("output/demo.log").string().c_str());
    return 0;
}
