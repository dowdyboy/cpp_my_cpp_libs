#pragma once
// =============================================================================
//  my::log —— 统一日志接口（底层 spdlog）
//
//  设计目标
//    * 迁移成本低：换掉 spdlog 时业务代码不动
//    * 零依赖头：本头不引入 spdlog / fmt，可放心放在公共 API 头里
//    * 未初始化可用：init() 之前调用也会落到 stderr，不会丢日志
//    * 自定义出口：set_sink() 可把日志导到串口/文件系统/RTT 等
//  格式化版接口见 <my_cpp_libs/log/format.hpp>（引入 fmt）。
// =============================================================================

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include "my_cpp_libs/core/status.hpp"
#include "my_cpp_libs/export.hpp"

namespace my::log {

// -----------------------------------------------------------------------------
//  等级
// -----------------------------------------------------------------------------
enum class Level : int { Trace = 0, Debug, Info, Warn, Error, Critical, Off };

MYLIB_API const char* level_name(Level level) noexcept;

// -----------------------------------------------------------------------------
//  配置
// -----------------------------------------------------------------------------
struct Config {
    /// logger 名称（spdlog 中用于 find/register）
    std::string logger_name = "mylib";

    /// 日志级别下限
    Level level = Level::Info;

    /// 是否输出到控制台（stdout）
    bool to_console = true;
    /// 控制台是否着色
    bool use_color = true;

    /// 文件输出路径；为空则不写文件
    std::string file_path;
    /// 滚动阈值（字节），0 表示不滚动
    std::size_t rotate_bytes = 10u * 1024u * 1024u;
    /// 保留的滚动文件数
    std::size_t rotate_files = 3;

    /// 异步日志（适合高吞吐；退出前必须 shutdown() 或 flush()）
    bool        async = false;
    std::size_t async_queue_size = 8192;
    /// 异步线程策略：block=队列满时阻塞，overrun_oldest=丢弃最旧
    bool async_block_on_full = true;

    /// 输出格式（spdlog pattern 语法）
    std::string pattern = "%Y-%m-%d %H:%M:%S.%e [%^%l%$] [%t] %v";
};

// -----------------------------------------------------------------------------
//  生命周期
// -----------------------------------------------------------------------------
/// 初始化。重复调用会先 shutdown 再重建。
MYLIB_API Status init(const Config& config);

/// 注销并释放全局 logger
MYLIB_API void shutdown();

MYLIB_API bool is_initialized() noexcept;

// -----------------------------------------------------------------------------
//  级别控制
// -----------------------------------------------------------------------------
MYLIB_API void  set_level(Level level) noexcept;
MYLIB_API Level level() noexcept;
/// 当前级别下该等级是否会被输出（用于避免昂贵的参数构造）
MYLIB_API bool  should_log(Level level) noexcept;

// -----------------------------------------------------------------------------
//  输出
// -----------------------------------------------------------------------------
/// 写出已格式化的消息。线程安全；未初始化时落到 stderr。
MYLIB_API void write(Level level, std::string_view message) noexcept;

MYLIB_API void flush() noexcept;

// -----------------------------------------------------------------------------
//  自定义出口
// -----------------------------------------------------------------------------
/// 自定义 sink：设置后所有日志（含文件/控制台）额外转发到这里。
/// 典型用途：Zynq 的 UART、Windows 调试器输出、GUI 日志面板。
using Sink = std::function<void(Level level, std::string_view message)>;

MYLIB_API void set_sink(Sink sink);
MYLIB_API void clear_sink();

/// 便捷判断 + 写出一行（等价于 should_log() + write()）
inline void write_if(Level level, std::string_view message) noexcept {
    if (should_log(level)) write(level, message);
}

}  // namespace my::log
