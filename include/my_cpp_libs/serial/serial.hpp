#pragma once
// =============================================================================
//  my::serial —— 串口（Windows COM / Linux termios），不依赖任何三方库
//
//  为什么自己写：
//    * 平台差异就这么几处 —— Windows 的 CreateFile/DCB/COMMTIMEOUTS/ClearCommError
//      对 Linux 的 open/termios/tcsetattr/FIONREAD，两边形状不同但都是"打开→配置→收发"；
//    * 板端（Zynq 上接 FPGA / 传感器 / GPS）要的就是这一套，引 Boost.Asio 是重依赖。
//
//  约定：
//    * **read_some() 超时返回 0 字节，不是错误**。串口"暂时没数据"是常态，轮询循环里
//      不该每次都当错误处理；只有真正的错误（未打开、设备消失、ioctl 失败、
//      **对端挂断/EOF**）才返回 Status。对端挂断/EOF 映射成 Errc::IoError，不是
//      0 字节超时 —— 否则文档推荐的 `if (n == 0) continue;` 会在拔线后 100% 空转。
//      （my::net 的 recv 超时返回 Errc::Timeout —— TCP 超时通常意味着对端出问题，
//        这两者的取舍不同，不要记混。）
//    * write_all() 会把整个缓冲写完，或超时后返回已写字节数与 Errc::Timeout。
//    * 析构自动 close；移动语义安全；**禁止拷贝**。线程安全：同一时刻只让一个线程读、
//      一个线程写，库内部不加锁。
//
//  波特率：
//    * Windows：任意整数（DCB.BaudRate 本身就接受任意值）；
//    * Linux：只支持 termios 的标准档位，见 standard_bauds()。给非标准值会返回
//      Errc::InvalidArgument 并在消息里列出支持的值 —— 不静默降级。
//
//  典型用法（C++17：逐字段赋值，不能用 C++20 的指定初始化器）：
//    my::serial::Config cfg;
//    cfg.baud      = 921600;
//    cfg.data_bits = 8;
//    auto port = my::serial::Port::open("/dev/ttyUSB0", cfg);
//    if (!port) { /* port.status().message() */ }
//    std::uint8_t buf[64];
//    auto n = port.value().read_some(buf, sizeof buf, std::chrono::milliseconds(100));
//    if (n && n.value() > 0) { ... }
// =============================================================================

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "my_cpp_libs/core/status.hpp"
#include "my_cpp_libs/export.hpp"

namespace my::serial {

// =============================================================================
//  配置
// =============================================================================
enum class Parity : std::uint8_t { None = 0, Even, Odd };

enum class StopBits : std::uint8_t { One = 0, Two };

/// 流控。Hardware = RTS/CTS；Software = XON/XOFF。
enum class Flow : std::uint8_t { None = 0, Hardware, Software };

struct Config {
    std::uint32_t baud      = 115200;
    std::uint8_t  data_bits = 8;  ///< 5..8（7 位常见于老设备；5/6 位由平台决定是否支持）
    Parity        parity    = Parity::None;
    StopBits      stop_bits = StopBits::One;
    Flow          flow      = Flow::None;
    /// POSIX：加 TIOCEXCL（独占，别的进程再打开会失败，避免两边抢同一个口）；
    /// Windows：CreateFile 的 dwShareMode=0（同样效果）。false 时允许其它进程同时打开
    /// （调试/监视场景；两边同时写会互相穿插，调用方自己负责）。
    bool exclusive = true;
};

/// 参数合法性检查（不碰设备）。open() 第一步就会调它。
MYLIB_API Status validate(const Config& cfg);

/// Linux 上 termios 支持的标准波特率（升序）。Windows 上返回空表：那里任意值都行。
MYLIB_API const std::vector<std::uint32_t>& standard_bauds();

/// Linux 上该波特率是否在标准档位里；Windows 恒为 true。
MYLIB_API bool is_baud_supported(std::uint32_t baud);

// =============================================================================
//  端口枚举
// =============================================================================
struct PortInfo {
    std::string name;         ///< 可直接传给 Port::open()："COM3" / "/dev/ttyUSB0"
    std::string description;  ///< 人类可读的补充信息（可能为空）
};

/// 列出当前系统上看起来可用的串口。
///
/// * Windows：读注册表 `HKLM\HARDWARE\DEVICEMAP\SERIALCOMM`（系统自己维护的 COM 映射，
///   不需要 setupapi），description 留空；
/// * Linux：扫 `/dev/tty{USB,ACM,S,AMA}*` 与 `/dev/rfcomm*`，并把 `/dev/serial/by-id/*`
///   作为带描述的条目附在后面（by-id 路径比 ttyUSB 稳定，适合写进配置文件）。
///
/// 返回空表不是错误：容器里本来就可能一个串口都没有。按名字排序。
MYLIB_API Result<std::vector<PortInfo>> list_ports();

// =============================================================================
//  Port
// =============================================================================
class MYLIB_API Port {
public:
    Port() noexcept = default;
    ~Port();

    Port(const Port&)            = delete;
    Port& operator=(const Port&) = delete;
    Port(Port&& other) noexcept;
    Port& operator=(Port&& other) noexcept;

    /// 打开并配置。先 validate(cfg)，再做平台相关的 open/配置。
    /// 设备不存在返回 Errc::NotFound；参数不支持（如 Linux 的非标准波特率）返回
    /// Errc::InvalidArgument；权限不足等返回 Errc::IoError（消息里带 errno/GetLastError）。
    [[nodiscard]] static Result<Port> open(std::string_view name, const Config& cfg = {});

    /// 已打开端口名（未打开时为空串）。
    const std::string& name() const noexcept { return name_; }
    bool               is_open() const noexcept { return handle_ >= 0; }

    /// 关闭。重复关闭、从未打开都返回 Ok（幂等）。
    Status close() noexcept;

    /// 读最多 n 字节；**超时返回 0 字节、status 为 Ok**（见文件头约定）。
    /// 对端挂断/EOF 返回 Errc::IoError，不是 0 字节超时。
    /// n == 0 立即返回 0。
    Result<std::size_t> read_some(void* buf, std::size_t n, std::chrono::milliseconds timeout);

    /// 尽量写出去，返回实际写入的字节数。**超时返回已写部分、status 仍为 Ok** ——
    /// 串口写超时通常只是"对端没读 / 流控压住"，是不是要重试由调用方决定。
    /// timeout < 0 表示无限等待；0 表示"尽量不阻塞"（Windows 用 1ms 分片，POSIX poll(0)）。
    /// 只有真正的错误（未打开、设备消失）才返回 Status。
    Result<std::size_t> write_some(const void* buf, std::size_t n,
                                   std::chrono::milliseconds timeout);

    /// 循环写直到写完；**timeout 是整次调用的总预算**，写不完返回 Errc::Timeout
    /// （消息里带 "已写 X/Y 字节"）。想自己控制重试节奏就用 write_some()。
    Status write_all(const void* buf, std::size_t n, std::chrono::milliseconds timeout);

    /// 读一行（不含行尾）。遇到 delim、读满 max_len、或超时即返回。
    /// 返回的字符串不含 delim；超时且一个字节都没读到时返回空串 + Ok。
    Result<std::string> read_line(char delim = '\n', std::size_t max_len = 4096,
                                  std::chrono::milliseconds timeout = std::chrono::seconds(1));

    /// 接收缓冲区里当前可读的字节数（FIONREAD / COMSTAT.cbInQue）。
    Result<std::size_t> bytes_available();

    /// 丢弃接收/发送缓冲里尚未处理的字节。
    /// Linux 用 tcflush(TCIOFLUSH)；Windows 用 PurgeComm。
    Status flush() noexcept;

    /// 引出一个原生句柄：POSIX 是文件描述符（int），Windows 是 HANDLE（转成 intptr_t）。
    /// 给"想用 select/poll 或 WaitForSingleObject 自己等"的调用方留的口子。
    std::intptr_t native_handle() const noexcept { return handle_; }

private:
    std::intptr_t handle_ = -1;
    std::string   name_;
};

}  // namespace my::serial
