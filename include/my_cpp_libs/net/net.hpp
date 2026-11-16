#pragma once
// =============================================================================
//  my::net —— TCP / UDP（跨 Windows / Linux，不依赖任何三方库）
//
//  为什么不引库：
//    * TCP/UDP 要的就是 BSD socket，Asio/libuv 解决的是"事件驱动 + 高并发"，
//      在板端（一条控制链路 + 一路遥测）属于杀鸡用牛刀；
//    * 真正麻烦的是平台差异，而差异只有这么多：
//        WSAStartup / SOCKET vs int / closesocket vs close / errno vs WSAGetLastError
//        SIGPIPE（Linux 写死连接会**杀进程**）/ 非阻塞 / 收发超时 / SO_REUSEADDR
//      这些集中吃在一个 .cpp 里就完了，引库反而多一层要学的东西。
//
//  约定：
//    * 所有失败都返回 Status，消息里带 errno/WSA 错误码；
//    * `recv()` 返回 0 表示**对端正常关闭**（和 POSIX 一致），出错才返回 Status；
//    * 默认阻塞；set_timeout() 之后读/写超时返回 Errc::Timeout；
//    * 析构自动 close，移动语义安全，禁止拷贝。
//
//  线程安全：单个 socket 不做内部加锁 —— 同一时刻只让一个线程读、一个线程写
//  （TCP 全双工是允许的），不要多线程同时 recv 同一个 socket。
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "my_cpp_libs/core/status.hpp"
#include "my_cpp_libs/export.hpp"

namespace my::net {

using Bytes = std::vector<std::uint8_t>;

// =============================================================================
//  地址
// =============================================================================
struct Endpoint {
    std::string   host;  ///< "192.168.1.10" / "localhost" / ""（表示 INADDR_ANY）
    std::uint16_t port = 0;

    std::string to_string() const;
    bool        empty() const noexcept { return host.empty() && port == 0; }
};

/// 解析 "host:port" / "[::1]:port" / ":8080" / "host"（配合 default_port）
MYLIB_API Result<Endpoint> parse_endpoint(std::string_view text, std::uint16_t default_port = 0);

// =============================================================================
//  TCP
// =============================================================================
class TcpSocket {
public:
    TcpSocket() noexcept = default;
    ~TcpSocket();
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;
    TcpSocket(const TcpSocket&)            = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    /// 连接（超时只作用于 connect 本身）。返回值不设收发超时，需要时再调 set_timeout()。
    MYLIB_API static Result<TcpSocket> connect(const Endpoint& ep, int timeout_ms = 2000);

    /// 设置收发超时；ms == 0 表示恢复阻塞
    MYLIB_API Status set_timeout(int ms);

    /// 单次发送：可能只写出去一部分，返回值是实际字节数
    MYLIB_API Result<std::size_t> send(const void* data, std::size_t size);

    /// 循环发送直到写完（处理部分写 + EINTR）；失败返回 Status
    MYLIB_API Status send_all(const void* data, std::size_t size);

    /// 单次接收：返回 0 表示对端已关闭连接
    MYLIB_API Result<std::size_t> recv(void* buffer, std::size_t size);

    /// 收满 size 字节；对端提前关闭 → IoError
    MYLIB_API Result<Bytes> recv_exact(std::size_t size);

    /// 半关闭写端（告诉对端"我不再发了"，读端仍然可用）
    MYLIB_API Status shutdown_write();

    MYLIB_API void close() noexcept;
    MYLIB_API bool valid() const noexcept;
    MYLIB_API int  native_handle() const noexcept;

    /// 对端地址（getsockname/getpeername 取到的实际值，失败返回空）
    MYLIB_API Endpoint peer() const;
    MYLIB_API Endpoint local() const;

private:
    friend class TcpListener;
    explicit TcpSocket(std::intptr_t fd) noexcept : fd_(fd) {}
    std::intptr_t fd_ = -1;
};

class TcpListener {
public:
    TcpListener() noexcept = default;
    ~TcpListener();
    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;
    TcpListener(const TcpListener&)            = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    /// 绑定监听。port = 0 时由系统分配（用 port() 取实际端口）。
    /// reuse_addr 默认开：避免服务端重启时 bind 失败（TIME_WAIT）。
    MYLIB_API static Result<TcpListener> bind(const Endpoint& ep, int backlog = 8,
                                              bool reuse_addr = true);

    /// 等待连接；timeout_ms < 0 表示一直等
    MYLIB_API Result<TcpSocket> accept(int timeout_ms = -1);

    /// 实际监听的端口（bind 时传 0 的话很有用）
    MYLIB_API std::uint16_t port() const;
    MYLIB_API void          close() noexcept;
    MYLIB_API bool          valid() const noexcept;

private:
    std::intptr_t fd_ = -1;
};

// =============================================================================
//  UDP
// =============================================================================
struct Datagram {
    Bytes    data;
    Endpoint from;
};

class UdpSocket {
public:
    UdpSocket() noexcept = default;
    ~UdpSocket();
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;
    UdpSocket(const UdpSocket&)            = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    /// 绑定到本地地址（host 为空表示 INADDR_ANY）
    MYLIB_API static Result<UdpSocket> bind(const Endpoint& ep);

    /// 只发不收的 socket（系统在第一次 send_to 时分配端口）
    MYLIB_API static Result<UdpSocket> open();

    MYLIB_API Status set_timeout(int ms);
    MYLIB_API Status set_broadcast(bool on);

    /// 加入/退出组播组；local_iface 为空表示用默认网卡
    MYLIB_API Status join_multicast(const std::string& group,
                                    const std::string& local_iface = {});
    MYLIB_API Status leave_multicast(const std::string& group,
                                     const std::string& local_iface = {});

    MYLIB_API Result<std::size_t> send_to(const Endpoint& ep, const void* data,
                                          std::size_t size);
    /// 收一个数据报；max_size 是缓冲上限
    MYLIB_API Result<Datagram> recv_from(std::size_t max_size = 64 * 1024);

    MYLIB_API std::uint16_t port() const;
    MYLIB_API void          close() noexcept;
    MYLIB_API bool          valid() const noexcept;

private:
    std::intptr_t fd_ = -1;
};

// =============================================================================
//  两种最常用的应用层分帧（板端 <-> 上位机自定义协议基本就这两种）
// =============================================================================
/// 读一行（到 '\n' 为止，自动去掉行尾 '\r'）。max_len 防止对端发垃圾把内存吃光。
MYLIB_API Status read_line(TcpSocket& sock, std::string& line, std::size_t max_len = 8192);

/// 4 字节大端长度前缀 + 负载
MYLIB_API Status write_frame(TcpSocket& sock, const void* data, std::size_t size);
MYLIB_API Status write_frame(TcpSocket& sock, const std::string& text);

/// 读一帧；max_size 防止伪造的长度字段撑爆内存
MYLIB_API Result<Bytes> read_frame(TcpSocket& sock, std::size_t max_size = 16 * 1024 * 1024);

}  // namespace my::net
