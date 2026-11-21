#include "my_cpp_libs/log/log.hpp"

#include "my_cpp_libs/core/path_utf8.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

namespace my::log {
namespace {

/// spdlog 的 file sink 在 Windows 上用 fopen(const char*)，按 ANSI 代码页解释路径。
/// 这会让中文/日文日志路径静默写到乱码目录（实测：init 返回 Ok，文件却不在期望位置）。
///
/// 这里自己实现一个 UTF-8 安全的文件 sink：
///   * 用 `core::to_path()` 打开文件（Windows 走 UTF-16）；
///   * 保留按大小滚动（rotate_bytes/rotate_files），文件命名与 spdlog 一致（base.N）；
///   * 继承 base_sink，加锁与 pattern 格式化仍由 spdlog 负责。
class Utf8FileSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    Utf8FileSink(std::string path, std::size_t max_bytes, std::size_t max_files)
        : path_(std::move(path)), max_bytes_(max_bytes), max_files_(max_files) {
        open_append_();
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        base_sink<std::mutex>::formatter_->format(msg, formatted);

        if (max_bytes_ > 0 && size_ > 0 && size_ + formatted.size() > max_bytes_) {
            rotate_();
        }
        out_.write(formatted.data(), static_cast<std::streamsize>(formatted.size()));
        if (!out_) {
            throw spdlog::spdlog_ex("写入日志文件失败: " + path_);
        }
        size_ += formatted.size();
    }

    void flush_() override {
        if (out_.is_open()) out_.flush();
    }

private:
    void open_append_() {
        out_.open(my::core::to_path(path_), std::ios::binary | std::ios::app);
        if (!out_) {
            throw spdlog::spdlog_ex("无法打开日志文件: " + path_);
        }
        std::error_code ec;
        const auto      sz = std::filesystem::file_size(my::core::to_path(path_), ec);
        size_              = ec ? 0 : static_cast<std::size_t>(sz);
    }

    std::string rotated_(std::size_t index) const {
        return path_ + "." + std::to_string(index);
    }

    void rotate_() {
        out_.flush();
        out_.close();
        if (max_files_ == 0) {  // 由 init() 拦住，这里只是兜底
            open_append_();
            return;
        }

        std::error_code ec;
        std::filesystem::remove(my::core::to_path(rotated_(max_files_)), ec);

        // base.1 -> base.2 -> ... -> base.N；Windows 上 rename 不覆盖，先删目标
        for (std::size_t i = max_files_; i > 1; --i) {
            const std::string from = rotated_(i - 1);
            const std::string to   = rotated_(i);
            ec.clear();
            if (!std::filesystem::exists(my::core::to_path(from), ec)) continue;
            ec.clear();
            std::filesystem::remove(my::core::to_path(to), ec);
            ec.clear();
            std::filesystem::rename(my::core::to_path(from), my::core::to_path(to), ec);
        }

        ec.clear();
        std::filesystem::remove(my::core::to_path(rotated_(1)), ec);
        ec.clear();
        std::filesystem::rename(my::core::to_path(path_), my::core::to_path(rotated_(1)), ec);
        if (ec) {
            // 滚动失败不应该丢当前这条日志：继续写回原文件，下一次再试
            std::fprintf(stderr, "[my_cpp_libs] 日志滚动失败: %s\n", ec.message().c_str());
        }
        open_append_();
    }

    std::string   path_;
    std::size_t   max_bytes_ = 0;
    std::size_t   max_files_ = 0;
    std::ofstream out_;
    std::size_t   size_ = 0;
};

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
    // **故意不析构**（有意泄漏这一个对象）。
    //
    // State 里有 mutex 和 shared_ptr<logger>。原来写成 `static State s;` 时，
    // 它会跟着静态析构顺序一起销毁；如果宿主在**自己的静态对象析构函数里**写日志
    // （很常见的"关闭时打最后一行"写法），就会去锁一个已经销毁的 mutex ——
    // 表现为退出时偶发崩溃或挂死，而且极难定位。
    //
    // log.hpp 承诺"任何阶段都可调用"，那就不能让它的生命周期短于进程。
    // 换成函数内静态指针后，进程退出时会直接 _exit 掉这块内存，不再析构。
    static State* s = new State();
    return *s;
}

/// 未初始化时的兜底输出：直写 stderr，不分配、不加锁（stdio 自带锁）
void fallback_write(Level level, std::string_view msg) noexcept {
    if (msg.empty()) return;
    // 用 fwrite 而不是 %.*s：日志可能含内嵌 NUL（二进制报文），且 %.*s 的
    // 精度是 int，>2GiB 会变成负值（读到 NUL 才停，越界读）。
    std::fprintf(stderr, "[%s] ", level_name(level));
    std::fwrite(msg.data(), 1, msg.size(), stderr);
    std::fputc('\n', stderr);
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
        // async_queue_size == 0 会让 spdlog 的 circular_q 变成"容量 1 且恒满"，
        // 第一条日志就永久阻塞。这是配置错误，必须在 init 阶段拒绝，不能等运行时挂死。
        if (config.async && config.async_queue_size == 0) {
            return Status(Errc::InvalidArgument,
                          "async_queue_size 必须 >= 1（0 会让异步日志在第一条消息处挂死）");
        }
        if (config.rotate_bytes > 0 && config.rotate_files == 0) {
            return Status(Errc::InvalidArgument,
                          "rotate_files 必须 >= 1（rotate_bytes > 0 时）");
        }

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
            // 用自实现的 UTF-8 安全 sink：spdlog 自带的 file sink 在 Windows 上
            // 按 ANSI 打开路径，中文/日文目录会静默写错位置（0.1.9 及之前）。
            sinks.push_back(std::make_shared<Utf8FileSink>(config.file_path, config.rotate_bytes,
                                                           config.rotate_files));
        }

        if (sinks.empty()) {
            return Status(Errc::InvalidArgument,
                          "日志配置既没有控制台也没有文件输出，至少要启用一个");
        }

        for (auto& sink : sinks) {
            try {
                sink->set_pattern(config.pattern);
            } catch (const std::exception& e) {
                return Status(Errc::InvalidArgument,
                              std::string("日志 pattern 非法: ") + e.what());
            }
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
        if (logger) logger->flush();
    } catch (...) {
        // 关闭路径不允许抛出
    }
    // 等异步队列排空再释放 logger。
    //
    // 队列里残留的 async_msg **各自持有一份 logger 的 shared_ptr**（spdlog 的实现），
    // 所以"主线程 reset logger"和"worker 线程析构最后一条消息"会并发碰到同一个对象：
    // 最后一个引用由谁释放不确定。shared_ptr 的引用计数是原子的（标准保证安全），
    // 但 TSan 会把它报成 "atomic 减引用 vs operator delete" 的 race —— CI 的
    // linux-tsan 在 log_async_roundtrip 上就报过一次。
    // spdlog 没有"停止"接口（post_async_msg_ 是私有的），只能按 queue_size() 做有界等待，
    // 把窗口压到最小；剩下的窄口子在 tests/tsan.supp 里做**针对性**抑制。
    if (pool) {
        for (int i = 0; i < 500 && pool->queue_size() > 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    logger.reset();
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
