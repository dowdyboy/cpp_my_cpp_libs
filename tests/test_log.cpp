#include "test_framework.hpp"

#include <my_cpp_libs/core/path_utf8.hpp>
#include <my_cpp_libs/log/log.hpp>

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <process.h>
#else
#  include <unistd.h>
#endif

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

    std::filesystem::remove(cfg.file_path, ec);  // 上次跑剩的清掉，否则下面数不准

    CHECK_OK(my::log::init(cfg));
    for (int i = 0; i < 100; ++i) my::log::write(my::log::Level::Info, "async 一行");
    my::log::flush();
    my::log::shutdown();
    CHECK(!my::log::is_initialized());

    // 关键：**回读文件**。以前这里只断言 is_initialized()==false，
    // 异步队列丢光/重复/截断都会绿 —— 那等于没测。
    std::ifstream in(cfg.file_path, std::ios::binary);
    CHECK(static_cast<bool>(in));
    std::size_t lines  = 0;
    std::string line;
    std::size_t hits   = 0;
    while (std::getline(in, line)) {
        ++lines;
        if (line.find("async 一行") != std::string::npos) ++hits;
    }
    CHECK_EQ(hits, std::size_t{100});   // 一条不多、一条不少
}

// A6 回归：异步 logger 的 flush() 必须把队列排空，而不是只把 flush 消息入队。
// 20 万行实测丢过最后 50 行；这里用 20000 行 + 唯一临时文件把窗口放大。
// 关键：flush() 返回后**立即**读文件，不能靠随后的 shutdown() 掩盖丢失。
MYTEST_CASE(log_async_flush_drains_queue) {
    std::error_code ec;
    std::filesystem::create_directories("output", ec);

#if defined(_WIN32)
    const unsigned long pid = static_cast<unsigned long>(::_getpid());
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    std::random_device rd;
    const std::string  path = "output/mylib_test_log_async_flush_" + std::to_string(pid) + "_" +
                             std::to_string(rd()) + ".log";
    std::filesystem::remove(my::core::to_path(path), ec);

    my::log::Config cfg;
    cfg.to_console       = false;
    cfg.file_path        = path;
    cfg.rotate_bytes     = 0;
    cfg.level            = my::log::Level::Info;
    cfg.async            = true;
    cfg.async_queue_size = 8192;
    cfg.pattern          = "%v";
    CHECK_OK(my::log::init(cfg));

    constexpr int kLines = 20000;
    for (int i = 0; i < kLines; ++i) {
        my::log::write(my::log::Level::Info, "async-flush-line-" + std::to_string(i));
    }
    my::log::flush();

    // 立即逐行读：此时异步队列应当已经排空
    std::size_t lines  = 0;
    bool        opened = false;
    {
        std::ifstream in(my::core::to_path(path), std::ios::binary);
        opened = static_cast<bool>(in);
        std::string line;
        while (std::getline(in, line)) ++lines;
    }
    my::log::shutdown();
    std::filesystem::remove(my::core::to_path(path), ec);

    CHECK(opened);
    CHECK_EQ(lines, std::size_t{kLines});   // 一条不能少
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

// -----------------------------------------------------------------------------
//  并发写入 / 关闭路径
// -----------------------------------------------------------------------------

namespace {

/// 整读一个小测试日志文件（sink 关掉之后读，内容才完整）
std::string read_whole_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::size_t count_lines(const std::string& text) {
    return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
}

}  // namespace

// log 声称"线程安全"的唯一实际验证：4 个线程往同一个文件 sink 里写，
// 一条都不能丢、也不能被交错成半行。
MYTEST_CASE(log_concurrent_writes) {
    const std::string path = "output/mylib_test_log_concurrent.log";
    std::error_code   ec;
    std::filesystem::create_directories("output", ec);
    // 文件 sink 是追加模式，上一次失败运行留下的旧内容会让行数对不上
    std::filesystem::remove(path, ec);

    my::log::Config cfg;
    cfg.to_console   = false;
    cfg.file_path    = path;
    cfg.rotate_bytes = 0;
    cfg.level        = my::log::Level::Info;
    CHECK_OK(my::log::init(cfg));

    constexpr int            kThreads   = 4;
    constexpr int            kPerThread = 2000;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([t] {
            for (int i = 0; i < kPerThread; ++i) {
                my::log::write(my::log::Level::Info,
                               "thread-log " + std::to_string(t) + ":" + std::to_string(i));
            }
        });
    }
    for (auto& w : workers) w.join();

    my::log::flush();
    my::log::shutdown();
    CHECK(!my::log::is_initialized());

    const std::string content = read_whole_file(path);
    CHECK_EQ(count_lines(content), std::size_t{kThreads * kPerThread});   // 8000 行一条不少
    CHECK(content.find("thread-log 0:0") != std::string::npos);          // 每个线程的首行都在
    CHECK(content.find("thread-log 3:1999") != std::string::npos);       // 也是每个线程的末行

    std::filesystem::remove(path, ec);
}

// 运行期反复切级别（Trace <-> Error）和持续写入并发：不能崩、不能卡死，
// 而且窗口结束后必须还能写出可见日志。
MYTEST_CASE(log_concurrent_level_toggle) {
    const std::string path = "output/mylib_test_log_level_toggle.log";
    std::error_code   ec;
    std::filesystem::create_directories("output", ec);
    std::filesystem::remove(path, ec);

    my::log::Config cfg;
    cfg.to_console   = false;
    cfg.file_path    = path;
    cfg.rotate_bytes = 0;
    cfg.level        = my::log::Level::Info;
    CHECK_OK(my::log::init(cfg));

    std::atomic<bool> stop{false};
    std::atomic<int>  toggles{0};

    std::thread toggler([&] {
        bool up = true;
        while (!stop.load(std::memory_order_relaxed)) {
            my::log::set_level(up ? my::log::Level::Trace : my::log::Level::Error);
            up = !up;
            toggles.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::vector<std::thread> writers;
    writers.reserve(3);
    for (int t = 0; t < 3; ++t) {
        writers.emplace_back([t, &stop] {
            for (int i = 0; !stop.load(std::memory_order_relaxed); ++i) {
                my::log::write(my::log::Level::Info,
                               "toggle-line " + std::to_string(t) + ":" + std::to_string(i));
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));   // 并发窗口约 200ms
    stop = true;
    toggler.join();
    for (auto& w : writers) w.join();

    // 窗口结束后：恢复成确定的级别，必须还能写出一条可见日志（sink 没被切坏）
    my::log::set_level(my::log::Level::Info);
    my::log::write(my::log::Level::Info, "toggle-window-closed");
    my::log::flush();
    my::log::shutdown();

    CHECK(toggles.load() >= 5);   // 切换线程确实在跑，而不是被调度饿死
    CHECK(!my::log::is_initialized());

    const std::string content = read_whole_file(path);
    CHECK(content.find("toggle-window-closed") != std::string::npos);

    std::filesystem::remove(path, ec);
}

// Off 级别必须在进 sink 之前就短路：文件里一个字节都不该有。
MYTEST_CASE(log_off_level_writes_nothing) {
    const std::string path = "output/mylib_test_log_off.log";
    std::error_code   ec;
    std::filesystem::create_directories("output", ec);
    std::filesystem::remove(path, ec);

    my::log::Config cfg;
    cfg.to_console   = false;
    cfg.file_path    = path;
    cfg.rotate_bytes = 0;
    cfg.level        = my::log::Level::Off;
    CHECK_OK(my::log::init(cfg));
    CHECK_EQ(static_cast<int>(my::log::level()), static_cast<int>(my::log::Level::Off));
    CHECK(!my::log::should_log(my::log::Level::Critical));   // 连 Critical 都不放行

    int sink_hits = 0;
    my::log::set_sink([&sink_hits](my::log::Level, std::string_view) { ++sink_hits; });

    my::log::write(my::log::Level::Critical, "off-level-critical");
    my::log::write(my::log::Level::Error, "off-level-error");
    my::log::write(my::log::Level::Info, "off-level-info");
    my::log::flush();
    my::log::clear_sink();
    my::log::shutdown();

    CHECK_EQ(sink_hits, 0);   // 自定义 sink 也不该被调用
    CHECK(std::filesystem::exists(path));   // 文件 sink 打开时就会建出空文件
    CHECK_EQ(std::filesystem::file_size(path), std::uintmax_t{0});   // 但一个字节都没写

    std::filesystem::remove(path, ec);
}

// 连续 shutdown() 必须幂等；关闭期间的写走 stderr 兜底；再 init 能恢复。
MYTEST_CASE(log_shutdown_idempotent) {
    const std::string path = "output/mylib_test_log_shutdown.log";
    std::error_code   ec;
    std::filesystem::create_directories("output", ec);
    std::filesystem::remove(path, ec);

    my::log::Config cfg;
    cfg.to_console   = false;
    cfg.file_path    = path;
    cfg.rotate_bytes = 0;
    cfg.level        = my::log::Level::Info;

    CHECK_OK(my::log::init(cfg));
    CHECK(my::log::is_initialized());
    my::log::write(my::log::Level::Info, "shutdown-idempotent-first");
    my::log::flush();

    my::log::shutdown();
    CHECK(!my::log::is_initialized());
    my::log::shutdown();   // 第二次必须是空操作，不能崩也不能重复 flush
    CHECK(!my::log::is_initialized());

    // 关闭后写日志走 stderr 兜底：不崩，也不能把 logger 偷偷装回来
    my::log::write(my::log::Level::Info, "after-shutdown-fallback");
    my::log::flush();
    CHECK(!my::log::is_initialized());

    // 再 init 一次能恢复（同一个文件：追加而不是截断）
    CHECK_OK(my::log::init(cfg));
    CHECK(my::log::is_initialized());
    my::log::write(my::log::Level::Info, "shutdown-idempotent-second");
    my::log::flush();
    my::log::shutdown();
    CHECK(!my::log::is_initialized());

    const std::string content = read_whole_file(path);
    CHECK(content.find("shutdown-idempotent-first") != std::string::npos);
    CHECK(content.find("shutdown-idempotent-second") != std::string::npos);
    // 兜底那条只进 stderr，不能混进文件
    CHECK(content.find("after-shutdown-fallback") == std::string::npos);
    CHECK_EQ(count_lines(content), std::size_t{2});

    std::filesystem::remove(path, ec);
}

// async_queue_size == 0 会让 spdlog 的队列恒满，第一条日志永久阻塞。
// init 必须在配置阶段就拒绝，而不是等运行时挂死。
MYTEST_CASE(log_rejects_zero_async_queue) {
    my::log::Config cfg;
    cfg.to_console      = true;
    cfg.async           = true;
    cfg.async_queue_size = 0;

    auto st = my::log::init(cfg);
    CHECK(!st);
    CHECK_EQ(st.code(), my::Errc::InvalidArgument);

    // 同一个错误配置不能把已经工作的 logger 拆掉
    my::log::Config good;
    good.to_console = false;
    good.file_path  = "output/test_log_keep_after_bad_init.log";
    good.rotate_bytes = 0;
    CHECK_OK(my::log::init(good));
    CHECK(my::log::is_initialized());

    auto bad = my::log::init(cfg);
    CHECK(!bad);
    CHECK(my::log::is_initialized());  // 旧配置仍然可用
    my::log::shutdown();
}

// 滚动文件分支在 0.1.9 之前零覆盖；自定义 UTF-8 sink 必须保持同样的滚动语义。
MYTEST_CASE(log_rotating_file) {
    std::error_code ec;
    std::filesystem::create_directories("output", ec);

    const std::string path = "output/test_log_rotate.log";
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path + ".1", ec);
    std::filesystem::remove(path + ".2", ec);

    my::log::Config cfg;
    cfg.to_console   = false;
    cfg.file_path    = path;
    cfg.rotate_bytes = 256;
    cfg.rotate_files = 2;
    cfg.level        = my::log::Level::Info;
    cfg.pattern      = "%v";

    CHECK_OK(my::log::init(cfg));
    for (int i = 0; i < 40; ++i) {
        my::log::write(my::log::Level::Info, "rotation-line-" + std::to_string(i));
    }
    my::log::flush();
    my::log::shutdown();

    CHECK(std::filesystem::exists(path));
    CHECK(std::filesystem::exists(path + ".1"));
    CHECK(std::filesystem::exists(path + ".2"));
    // 文件总数不能超过 rotate_files + 1（base + .1 + .2）
    CHECK(!std::filesystem::exists(path + ".3"));

    std::filesystem::remove(path, ec);
    std::filesystem::remove(path + ".1", ec);
    std::filesystem::remove(path + ".2", ec);
}

// Windows 上 spdlog 自带的 file sink 按 ANSI 打开路径，中文目录会写到乱码位置。
// 自定义 sink 必须真正在期望路径落下文件。
MYTEST_CASE(log_utf8_path_roundtrip) {
    std::error_code ec;
    const std::string dir = "output/中文日志";
    std::filesystem::create_directories(my::core::to_path(dir), ec);
    CHECK(!ec);

    const std::string path = dir + "/app.log";
    std::filesystem::remove(my::core::to_path(path), ec);

    my::log::Config cfg;
    cfg.to_console   = false;
    cfg.file_path    = path;
    cfg.rotate_bytes = 0;
    cfg.pattern      = "%v";
    CHECK_OK(my::log::init(cfg));
    my::log::write(my::log::Level::Info, "utf8-path-line");
    my::log::flush();
    my::log::shutdown();

    CHECK(std::filesystem::exists(my::core::to_path(path)));
    {
        std::ifstream in(my::core::to_path(path), std::ios::binary);
        CHECK(static_cast<bool>(in));
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(text.find("utf8-path-line") != std::string::npos);
    }

    std::filesystem::remove(my::core::to_path(path), ec);
    std::filesystem::remove(my::core::to_path(dir), ec);
}
