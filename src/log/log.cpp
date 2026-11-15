#include "my_cpp_libs/log/log.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

namespace my::log {
namespace {

spdlog::level::level_enum to_spdlog(Level level) noexcept {
    switch (level) {
        case Level::Trace: return spdlog::level::trace;
        case Level::Debug: return spdlog::level::debug;
        case Level::Info: return spdlog::level::info;
        case Level::Warn: return spdlog::level::warn;
        case Level::Error: return spdlog::level::err;
        case Level::Critical: return spdlog::level::critical;
        case Level::Off: default: return spdlog::level::off;
    }
}

struct State {
    std::mutex                       mutex;
    std::shared_ptr<spdlog::logger>  logger;
    /// 异步模式下本库自有的线程池（不复用 spdlog 的全局池，避免和宿主互相覆盖）
    std::shared_ptr<spdlog::details::thread_pool> pool;
    std::atomic<int>                 level{static_cast<int>(Level::Info)};
    Sink                             sink;
    bool                             initialized = false;
};

State& state() {
    static State s;
    return s;
}

/// 未初始化时的兜底输出：直写 stderr，不分配、不加锁（stdio 自带锁）
void fallback_write(Level level, std::string_view msg) noexcept {
    if (msg.empty()) return;
    std::fprintf(stderr, "[%s] %.*s\n", level_name(level), static_cast<int>(msg.size()), msg.data());
    std::fflush(stderr);
}

}  // namespace

const char* level_name(Level level) noexcept {
    switch (level) {
        case Level::Trace: return "trace";
        case Level::Debug: return "debug";
        case Level::Info: return "info";
        case Level::Warn: return "warn";
        case Level::Error: return "error";
        case Level::Critical: return "critical";
        case Level::Off: return "off";
    }
    return "unknown";
}

Status init(const Config& config) {
    try {
        // ---- 先在局部把新 logger 完整搭好；任何一步失败都不动现有配置 ----
        std::vector<spdlog::sink_ptr> sinks;

        if (config.to_console) {
            if (config.use_color) {
                sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
            } else {
                sinks.push_back(std::make_shared<spdlog::sinks::stdout_sink_mt>());
            }
        }

        if (!config.file_path.empty()) {
            if (config.rotate_bytes > 0) {
                sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    config.file_path, config.rotate_bytes, config.rotate_files));
            } else {
                sinks.push_back(
                    std::make_shared<spdlog::sinks::basic_file_sink_mt>(config.file_path, false));
            }
        }

        if (sinks.empty()) {
            return Status(Errc::InvalidArgument,
                          "日志配置既没有控制台也没有文件输出，至少要启用一个");
        }

        for (auto& sink : sinks) {
            sink->set_pattern(config.pattern);
            sink->set_level(to_spdlog(config.level));
        }

        std::shared_ptr<spdlog::details::thread_pool> pool;
        std::shared_ptr<spdlog::logger>              logger;
        if (config.async) {
            // 自建线程池：不走 spdlog::init_thread_pool，避免覆盖宿主的全局池
            pool = std::make_shared<spdlog::details::thread_pool>(config.async_queue_size, 1);
            auto policy = config.async_block_on_full ? spdlog::async_overflow_policy::block
                                                     : spdlog::async_overflow_policy::overrun_oldest;
            logger = std::make_shared<spdlog::async_logger>(config.logger_name, sinks.begin(),
                                                            sinks.end(), pool, policy);
        } else {
            logger = std::make_shared<spdlog::logger>(config.logger_name, sinks.begin(), sinks.end());
        }

        logger->set_level(to_spdlog(config.level));
        logger->flush_on(spdlog::level::warn);

        // ---- 替换现有配置：旧对象必须在锁外销毁（异步池析构会 join 线程）----
        State& st = state();
        std::shared_ptr<spdlog::details::thread_pool> old_pool;
        std::shared_ptr<spdlog::logger>              old_logger;  // 先于 pool 销毁
        {
            std::lock_guard<std::mutex> lock(st.mutex);
            old_logger     = std::move(st.logger);
            old_pool       = std::move(st.pool);
            st.logger      = std::move(logger);
            st.pool        = std::move(pool);
            st.initialized = true;
        }
        st.level.store(static_cast<int>(config.level));

        if (old_logger) {
            try {
                old_logger->flush();
            } catch (...) {
                // 旧 logger 的刷新失败不影响新配置生效
            }
            old_logger.reset();
        }
        old_pool.reset();

        return Ok();
    } catch (const std::exception& e) {
        return Status(Errc::IoError, std::string("日志初始化失败: ") + e.what());
    } catch (...) {
        return Status(Errc::IoError, "日志初始化失败: 未知错误");
    }
}

void shutdown() {
    State& st = state();
    std::shared_ptr<spdlog::details::thread_pool> pool;
    std::shared_ptr<spdlog::logger>              logger;
    {
        std::lock_guard<std::mutex> lock(st.mutex);
        if (!st.initialized && !st.logger && !st.pool) return;
        logger         = std::move(st.logger);
        pool           = std::move(st.pool);
        st.initialized = false;
    }
    try {
        if (logger) {
            logger->flush();
            logger.reset();
        }
    } catch (...) {
        // 关闭路径不允许抛出
    }
    // 只回收本库自己创建的线程池。
    // 这里**不能**调 spdlog::shutdown()：那是进程级操作，会把宿主自己
    // register_logger() 过的 logger 和全局线程池一起清掉。
    pool.reset();
}

bool is_initialized() noexcept {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mutex);
    return st.initialized;
}

void set_level(Level level) noexcept {
    State& st = state();
    st.level.store(static_cast<int>(level));
    // 必须同时改 sink 的级别：spdlog 的 sink 自己也会过滤，
    // 只改 logger 级别的话，运行期"降级到 Debug"会被 sink 静默丢掉。
    try {
        std::lock_guard<std::mutex> lock(st.mutex);
        if (st.logger) {
            st.logger->set_level(to_spdlog(level));
            for (auto& sink : st.logger->sinks()) {
                if (sink) sink->set_level(to_spdlog(level));
            }
        }
    } catch (...) {
    }
}

Level level() noexcept { return static_cast<Level>(state().level.load()); }

bool should_log(Level level) noexcept {
    if (level == Level::Off) return false;
    return static_cast<int>(level) >= state().level.load();
}

void write(Level level, std::string_view message) noexcept {
    if (!should_log(level)) return;

    try {
        State& st = state();
        std::shared_ptr<spdlog::logger> logger;
        {
            std::lock_guard<std::mutex> lock(st.mutex);
            logger = st.logger;
        }
        if (logger) {
            logger->log(to_spdlog(level), spdlog::string_view_t(message.data(), message.size()));
        } else {
            fallback_write(level, message);
        }

        Sink sink;
        {
            std::lock_guard<std::mutex> lock(st.mutex);
            sink = st.sink;
        }
        if (sink) sink(level, message);
    } catch (...) {
        // 日志失败绝不能影响主流程
    }
}

void flush() noexcept {
    try {
        State& st = state();
        std::lock_guard<std::mutex> lock(st.mutex);
        if (st.logger) st.logger->flush();
    } catch (...) {
    }
}

void set_sink(Sink sink) {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mutex);
    st.sink = std::move(sink);
}

void clear_sink() {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.mutex);
    st.sink = nullptr;
}

}  // namespace my::log
