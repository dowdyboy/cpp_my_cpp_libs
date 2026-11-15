#include "test_framework.hpp"

#include <my_cpp_libs/log/log.hpp>

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <string>
#include <vector>

MYTEST_SUITE("log");

MYTEST_CASE(log_level_names) {
    CHECK_EQ(std::string(my::log::level_name(my::log::Level::Trace)), std::string("trace"));
    CHECK_EQ(std::string(my::log::level_name(my::log::Level::Info)), std::string("info"));
    CHECK_EQ(std::string(my::log::level_name(my::log::Level::Critical)), std::string("critical"));
    CHECK_EQ(std::string(my::log::level_name(my::log::Level::Off)), std::string("off"));
}

MYTEST_CASE(log_init_and_levels) {
    std::error_code ec;
    std::filesystem::create_directories("output", ec);

    my::log::Config cfg;
    cfg.to_console  = false;
    cfg.file_path   = "output/test_log.log";
    cfg.rotate_bytes = 0;
    cfg.level       = my::log::Level::Debug;

    CHECK_OK(my::log::init(cfg));
    CHECK(my::log::is_initialized());
    CHECK_EQ(static_cast<int>(my::log::level()), static_cast<int>(my::log::Level::Debug));

    CHECK(my::log::should_log(my::log::Level::Debug));
    CHECK(my::log::should_log(my::log::Level::Error));
    CHECK(!my::log::should_log(my::log::Level::Trace));
    CHECK(!my::log::should_log(my::log::Level::Off));

    my::log::set_level(my::log::Level::Warn);
    CHECK(!my::log::should_log(my::log::Level::Info));
    CHECK(my::log::should_log(my::log::Level::Warn));
    CHECK(my::log::should_log(my::log::Level::Critical));

    my::log::write(my::log::Level::Warn, "warn 一行");
    my::log::write(my::log::Level::Error, "error 一行");
    my::log::flush();
    my::log::shutdown();

    CHECK(!my::log::is_initialized());
}

MYTEST_CASE(log_sink_capture) {
    std::error_code ec;
    std::filesystem::create_directories("output", ec);

    my::log::Config cfg;
    cfg.to_console = false;
    cfg.file_path  = "output/test_log_sink.log";
    cfg.rotate_bytes = 0;
    cfg.level      = my::log::Level::Info;
    CHECK_OK(my::log::init(cfg));

    std::vector<std::string> captured;
    my::log::set_sink([&captured](my::log::Level level, std::string_view msg) {
        captured.push_back(std::string(my::log::level_name(level)) + ":" + std::string(msg));
    });

    my::log::write(my::log::Level::Info, "hello");
    my::log::write(my::log::Level::Error, "boom");
    // 低于阈值的不会进入 sink
    my::log::write(my::log::Level::Debug, "hidden");

    CHECK_EQ(captured.size(), std::size_t{2});
    CHECK_EQ(captured[0], std::string("info:hello"));
    CHECK_EQ(captured[1], std::string("error:boom"));

    my::log::clear_sink();
    my::log::write(my::log::Level::Info, "no sink now");
    CHECK_EQ(captured.size(), std::size_t{2});

    my::log::shutdown();
}

MYTEST_CASE(log_runtime_level_change) {
    std::error_code ec;
    std::filesystem::create_directories("output", ec);

    my::log::Config cfg;
    cfg.to_console   = false;
    cfg.file_path    = "output/test_log_level.log";
    cfg.rotate_bytes = 0;
    cfg.level        = my::log::Level::Warn;
    CHECK_OK(my::log::init(cfg));

    int captured = 0;
    my::log::set_sink([&captured](my::log::Level, std::string_view) { ++captured; });

    my::log::write(my::log::Level::Info, "信息级，当前阈值 Warn -> 应被过滤");
    CHECK_EQ(captured, 0);

    // 运行期降级必须同时作用到 sink，否则消息会被 sink 静默丢掉
    my::log::set_level(my::log::Level::Debug);
    my::log::write(my::log::Level::Debug, "调试级，现在应该能出来");
    CHECK_EQ(captured, 1);

    my::log::write(my::log::Level::Trace, "trace 仍低于 Debug -> 被过滤");
    CHECK_EQ(captured, 1);

    my::log::clear_sink();
    my::log::shutdown();
}

MYTEST_CASE(log_init_without_sink_fails) {
    my::log::Config cfg;
    cfg.to_console = false;
    cfg.file_path.clear();
    cfg.level = my::log::Level::Info;

    CHECK_ERR(my::log::init(cfg));
}

MYTEST_CASE(log_before_init_is_safe) {
    // 未初始化时写日志不能崩溃（落到 stderr）
    my::log::shutdown();
    CHECK(!my::log::is_initialized());
    my::log::write(my::log::Level::Info, "这条会走 stderr 兜底");
    my::log::flush();
}

// 重复 init 时若新配置非法，旧配置必须继续可用（不能先拆后建）
MYTEST_CASE(log_failed_reinit_keeps_old_config) {
    std::error_code ec;
    std::filesystem::create_directories("output", ec);

    my::log::Config good;
    good.to_console   = false;
    good.file_path    = "output/test_log_keep.log";
    good.rotate_bytes = 0;
    good.level        = my::log::Level::Warn;
    CHECK_OK(my::log::init(good));
    CHECK(my::log::is_initialized());

    my::log::Config bad;
    bad.to_console = false;
    bad.file_path.clear();
    CHECK_ERR(my::log::init(bad));

    // 旧配置仍在：状态、级别、写入路径都没被拆掉
    CHECK(my::log::is_initialized());
    CHECK_EQ(static_cast<int>(my::log::level()), static_cast<int>(my::log::Level::Warn));
    my::log::write(my::log::Level::Warn, "旧配置仍然可用");
    my::log::flush();

    my::log::shutdown();
}

MYTEST_CASE(log_async_roundtrip) {
    std::error_code ec;
    std::filesystem::create_directories("output", ec);

    my::log::Config cfg;
    cfg.to_console       = false;
    cfg.file_path        = "output/test_log_async.log";
    cfg.rotate_bytes     = 0;
    cfg.level            = my::log::Level::Info;
    cfg.async            = true;
    cfg.async_queue_size = 256;

    CHECK_OK(my::log::init(cfg));
    for (int i = 0; i < 100; ++i) my::log::write(my::log::Level::Info, "async 一行");
    my::log::flush();
    my::log::shutdown();
    CHECK(!my::log::is_initialized());
}

// 本库的 init/shutdown 只管理自己创建的 logger：
// 不能调用 spdlog::shutdown()，否则宿主 register 过的 logger 会被一起清掉。
MYTEST_CASE(log_does_not_disturb_host_spdlog) {
    std::error_code ec;
    std::filesystem::create_directories("output", ec);

    auto host = spdlog::create<spdlog::sinks::null_sink_mt>("host-owned");
    CHECK(spdlog::get("host-owned") != nullptr);

    my::log::Config cfg;
    cfg.to_console   = false;
    cfg.file_path    = "output/test_log_coexist.log";
    cfg.rotate_bytes = 0;
    CHECK_OK(my::log::init(cfg));
    my::log::write(my::log::Level::Info, "mylib 写一行");
    my::log::shutdown();

    CHECK(spdlog::get("host-owned") != nullptr);
    host->info("宿主 logger 仍然可用");
    host->flush();
    spdlog::drop("host-owned");
}
