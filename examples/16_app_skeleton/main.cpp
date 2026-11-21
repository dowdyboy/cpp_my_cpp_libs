// =============================================================================
//  一个"最小可用服务骨架"：配置 → 日志 → 文件 → 计时 → 线程池
//
//  这个示例的意义不是演示某个单个 API，而是演示这五个组件**串起来**该怎么写：
//   1. 配置：默认值 ← config.json ← 环境变量 ← 命令行
//   2. 日志：级别来自配置，非控制台场景可以直接写文件
//   3. io  ：原子写结果文件（断电不会留半截）
//   4. time：用单调时钟测耗时（不是系统时钟）
//   5. thread：线程池跑批 + 周期任务做心跳
//
//  运行：
//      example_app_skeleton
//      example_app_skeleton --worker.count=4 --worker.iterations=2000
//      MYAPP__WORKER__COUNT=8 example_app_skeleton     # Linux/macOS
//      $env:MYAPP__WORKER__COUNT=8; .\example_app_skeleton.exe   # PowerShell
// =============================================================================
#include <my_cpp_libs/io/io.hpp>
#include <my_cpp_libs/json/config.hpp>
#include <my_cpp_libs/log/format.hpp>
#include <my_cpp_libs/log/log.hpp>
#include <my_cpp_libs/thread/thread.hpp>
#include <my_cpp_libs/time/time.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    // ---------------------------------------------------------------- 1. 配置
    my::json::Document defaults;
    (void)defaults.set("app.name", "skeleton");
    (void)defaults.set("log.level", "info");
    (void)defaults.set("log.file", "");
    (void)defaults.set("worker.count", 2);
    (void)defaults.set("worker.iterations", 1000);
    (void)defaults.set("output.path", "skeleton_result.json");

    my::json::Layered cfg;
    if (auto st = cfg.add_defaults(std::move(defaults)); !st) {
        std::printf("默认配置合并失败: %s\n", st.to_string().c_str());
        return 1;
    }

    // 文件不存在不算错；存在但写坏了才报错
    if (auto st = cfg.add_file_if_exists("config.json"); !st) {
        std::printf("config.json 读取失败: %s\n", st.to_string().c_str());
        return 1;
    }
    // 环境变量：MYAPP__WORKER__COUNT=8 → worker.count
    if (auto st = cfg.add_env("MYAPP"); !st) {
        std::printf("环境变量解析失败: %s\n", st.to_string().c_str());
        return 1;
    }
    // 命令行：--worker.count=4
    if (auto st = cfg.add_argv(argc, argv); !st) {
        std::printf("命令行解析失败: %s\n", st.to_string().c_str());
        return 1;
    }

    const std::string app_name = cfg.value<std::string>("app.name", "app");
    const int         workers  = cfg.value<int>("worker.count", 2);
    const int         iters    = cfg.value<int>("worker.iterations", 1000);
    const std::string out_path = cfg.value<std::string>("output.path", "result.json");
    const std::string log_file = cfg.value<std::string>("log.file", "");

    // ---------------------------------------------------------------- 2. 日志
    my::log::Config log_cfg;
    log_cfg.pattern    = "[%Y-%m-%d %H:%M:%S.%e] [%l] %v";
    log_cfg.to_console = true;
    if (!log_file.empty()) log_cfg.file_path = log_file;
    if (auto st = my::log::init(log_cfg); !st) {
        std::printf("日志初始化失败: %s\n", st.to_string().c_str());
        return 1;
    }

    my::log::info("{} 启动，配置层数={} worker.count={} worker.iterations={}", app_name,
                  cfg.layers().size(), workers, iters);

    // 配置从哪来：排查"这个值到底谁设的"很有用
    for (const auto& layer : cfg.layers()) {
        static const char* kind_names[] = {"defaults", "file", "env", "argv", "manual"};
        my::log::debug("  层[{}] {} ({})", static_cast<int>(layer.kind),
                       layer.name.empty() ? "-" : layer.name,
                       kind_names[static_cast<int>(layer.kind)]);
    }

    // ---------------------------------------------------------------- 3. 线程池
    auto pool_result = my::thread::ThreadPool::create(static_cast<std::size_t>(workers));
    if (!pool_result) {
        my::log::error("线程池创建失败: {}", pool_result.status().to_string());
        return 1;
    }
    auto pool = std::move(pool_result.value());

    my::time::Stopwatch total;
    std::atomic<std::int64_t> sum{0};

    {
        my::time::ScopedTimer timer([&](double s) {
            my::log::info("批处理完成，耗时 {}", my::time::format_elapsed(s));
        });

        for (int i = 0; i < workers; ++i) {
            (void)pool->submit([i, iters, &sum] {
                std::int64_t local = 0;
                for (int k = 0; k < iters; ++k) local += (k % 7) * (i + 1);
                sum.fetch_add(local);
            });
        }
        if (!pool->wait_idle(std::chrono::milliseconds(5000))) {
            my::log::warn("等待任务完成超时（pending={}）", pool->pending());
        }
    }  // ScopedTimer 在这里打日志

    // ---------------------------------------------------------------- 4. 周期任务（心跳）
    std::atomic<int> heartbeats{0};
    auto heartbeat = my::thread::PeriodicTask::create(std::chrono::milliseconds(50), [&] {
        heartbeats.fetch_add(1);
    });
    if (!heartbeat) {
        my::log::error("周期任务创建失败: {}", heartbeat.status().to_string());
        return 1;
    }
    my::time::sleep_ms(180);
    heartbeat.value()->stop();
    my::log::info("心跳 {} 次，ticks={}", heartbeats.load(), heartbeat.value()->ticks());

    pool->shutdown();

    // ---------------------------------------------------------------- 5. 原子写结果
    my::json::Document result;
    (void)result.set("app", app_name);
    (void)result.set("sum", sum.load());
    (void)result.set("workers", workers);
    (void)result.set("iterations", iters);
    (void)result.set("heartbeats", heartbeats.load());
    (void)result.set("elapsed_ms", total.elapsed_ms());
    (void)result.set("finished_at", my::time::format_utc(my::time::unix_ms()));

    const std::string text = result.dump(2) + "\n";
    if (auto st = my::io::write_file(out_path, text); !st) {
        my::log::error("写结果失败: {}", st.to_string());
        my::log::shutdown();
        return 1;
    }
    my::log::info("结果已原子写入 {}（{} 字节）", out_path, text.size());

    my::log::shutdown();
    return 0;
}
