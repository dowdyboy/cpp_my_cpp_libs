#include "my_cpp_libs/net/net.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0A00
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>

using native_t = SOCKET;
static constexpr native_t kInvalidFd = INVALID_SOCKET;
static constexpr int      kShutWr    = SD_SEND;
#  define MYLIB_SOCK_ERRNO() WSAGetLastError()
#  define MYLIB_WOULD_BLOCK(e) ((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS)
#  define MYLIB_INTERRUPTED(e) ((e) == WSAEINTR)
#  define MYLIB_IN_PROGRESS(e) ((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS)
#  define MYLIB_TIMED_OUT(e) ((e) == WSAETIMEDOUT)
#  define MYLIB_EINVAL_VALUE WSAEINVAL
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>

using native_t = int;
static constexpr native_t kInvalidFd = -1;
static constexpr int      kShutWr    = SHUT_WR;
#  define MYLIB_SOCK_ERRNO() errno
#  define MYLIB_WOULD_BLOCK(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
#  define MYLIB_INTERRUPTED(e) ((e) == EINTR)
#  define MYLIB_IN_PROGRESS(e) ((e) == EINPROGRESS || (e) == EWOULDBLOCK)
#  define MYLIB_TIMED_OUT(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
#  define MYLIB_EINVAL_VALUE EINVAL

// macOS/BSD 没有 MSG_NOSIGNAL（用 SO_NOSIGPIPE）；这里兜底成 0，
// 并在建立 socket 时统一设 SO_NOSIGPIPE（见下面的 no_sigpipe() 调用）。
#  if !defined(MSG_NOSIGNAL)
#    define MSG_NOSIGNAL 0
#  endif
#endif

namespace my::net {
namespace {

/// 单次缓冲区分配上限。recv_exact()/read_frame() 的 size 来自外部报文，
/// 没有上限时一个畸形长度头就能让进程抛 bad_alloc（异常穿出 Result 边界）。
constexpr std::size_t kMaxBufferBytes = 256u * 1024u * 1024u;  // 256 MiB

// ----------------------------------------------------------------------------- 平台小工具
void ensure_wsa() {
#if defined(_WIN32)
    static struct WsaGuard {
        WsaGuard() {
            WSADATA d;
            WSAStartup(MAKEWORD(2, 2), &d);
        }
        ~WsaGuard() { WSACleanup(); }
    } guard;
    (void)guard;
#endif
}

std::string errno_text(int e) {
#if defined(_WIN32)
    switch (e) {
        case WSAECONNREFUSED: return "连接被拒绝(WSAECONNREFUSED)";
        case WSAETIMEDOUT: return "超时(WSAETIMEDOUT)";
        case WSAENETUNREACH: return "网络不可达(WSAENETUNREACH)";
        case WSAEHOSTUNREACH: return "主机不可达(WSAEHOSTUNREACH)";
        case WSAEADDRINUSE: return "地址已被占用(WSAEADDRINUSE)";
        case WSAECONNRESET: return "连接被重置(WSAECONNRESET)";
        case WSAEACCES: return "权限不足(WSAEACCES)";
        default: return "socket 错误 " + std::to_string(e);
    }
#else
    return std::string(std::strerror(e)) + " (errno=" + std::to_string(e) + ")";
#endif
}

Status sock_status(const char* what) {
    const int e = MYLIB_SOCK_ERRNO();
    if (MYLIB_TIMED_OUT(e)) {
        return Status(Errc::Timeout, std::string(what) + " 超时: " + errno_text(e));
    }
    return Status(Errc::IoError, std::string(what) + " 失败: " + errno_text(e));
}

/// 有 SO_NOSIGPIPE 的平台（macOS/BSD）在 socket 上关掉 SIGPIPE；
/// Linux 用 send() 的 MSG_NOSIGNAL，Windows 本来就没有 SIGPIPE。
void no_sigpipe(native_t fd) {
#if defined(SO_NOSIGPIPE)
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&one), sizeof(one));
#else
    (void)fd;
#endif
}

void close_fd(native_t fd) {
#if defined(_WIN32)
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}

bool set_blocking(native_t fd, bool blocking) {
#if defined(_WIN32)
    u_long mode = blocking ? 0 : 1;
    return ::ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    const int next = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    return ::fcntl(fd, F_SETFL, next) == 0;
#endif
}

#if !defined(_WIN32)
namespace {

/// 把错误码写回 errno（只有 POSIX 的 FD_SETSIZE 检查需要它）。
void set_sock_errno(int err) noexcept { errno = err; }

}  // namespace
#endif

/// 1 = 就绪，0 = 超时，-1 = 出错。timeout_ms < 0 表示一直等。
int wait_ready(native_t fd, bool for_write, int timeout_ms) {
#    if !defined(_WIN32)
    // POSIX 的 fd 是数组下标：fd >= FD_SETSIZE 时 FD_SET 会**栈越界**，必须挡住。
    // （Windows 的 fd_set 是"最多 FD_SETSIZE 个 SOCKET"的数组，SOCKET 的数值
    //   大小与 FD_SETSIZE 无关 —— 在那里照搬这个判断会把所有连接都拒掉。）
    if (static_cast<std::size_t>(fd) >= static_cast<std::size_t>(FD_SETSIZE)) {
        set_sock_errno(MYLIB_EINVAL_VALUE);
        return -1;
    }
#    endif
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);

    timeval tv{};
    timeval* ptv = nullptr;
    if (timeout_ms >= 0) {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv        = &tv;
    }
    for (;;) {
#if defined(_WIN32)
        const int rc = ::select(0, for_write ? nullptr : &set, for_write ? &set : nullptr, nullptr, ptv);
#else
        const int rc = ::select(static_cast<int>(fd) + 1, for_write ? nullptr : &set,
                                for_write ? &set : nullptr, nullptr, ptv);
#endif
        if (rc > 0) return 1;
        if (rc == 0) return 0;
        if (!MYLIB_INTERRUPTED(MYLIB_SOCK_ERRNO())) return -1;
        // 被信号打断：重试（timeval 可能被改写，按原值重设）
        if (timeout_ms >= 0) {
            tv.tv_sec  = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
        }
    }
}

struct AddrInfo {
    sockaddr_storage addr{};
    socklen_t        len = 0;
    int              family = AF_UNSPEC;
};

/// getaddrinfo 包装：passive = 用于 bind（host 为空表示任意地址）
Result<std::vector<AddrInfo>> resolve(const Endpoint& ep, bool passive, int socktype) {
    addrinfo h{};
    h.ai_family   = AF_UNSPEC;
    h.ai_socktype = socktype;
    if (passive) h.ai_flags |= AI_PASSIVE;

    const std::string service = std::to_string(ep.port);
    addrinfo*     raw         = nullptr;
    const char*   host        = ep.host.empty() ? nullptr : ep.host.c_str();
    const int     rc          = ::getaddrinfo(host, service.c_str(), &h, &raw);
    if (rc != 0) {
#if defined(_WIN32)
        return Status(Errc::InvalidArgument,
                      "解析地址失败: " + (ep.host.empty() ? std::string("*") : ep.host) + ":" +
                          service + " (getaddrinfo=" + std::to_string(rc) + ")");
#else
        return Status(Errc::InvalidArgument,
                      "解析地址失败: " + (ep.host.empty() ? std::string("*") : ep.host) + ":" +
                          service + " (" + ::gai_strerror(rc) + ")");
#endif
    }

    std::vector<AddrInfo> out;
    for (addrinfo* it = raw; it != nullptr; it = it->ai_next) {
        AddrInfo a;
        if (it->ai_addrlen > sizeof(a.addr)) continue;
        std::memcpy(&a.addr, it->ai_addr, it->ai_addrlen);
        a.len    = static_cast<socklen_t>(it->ai_addrlen);
        a.family = it->ai_family;
        out.push_back(a);
    }
    ::freeaddrinfo(raw);
    if (out.empty()) {
        return Status(Errc::InvalidArgument, "地址解析结果为空: " + ep.host);
    }
    return out;
}

uint16_t port_of(const sockaddr_storage& addr) {
    if (addr.ss_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(&addr);
        return ntohs(in->sin_port);
    }
    if (addr.ss_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        return ntohs(in6->sin6_port);
    }
    return 0;
}

std::string ip_of(const sockaddr_storage& addr) {
    char buf[INET6_ADDRSTRLEN] = {0};
    if (addr.ss_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(&addr);
        if (::inet_ntop(AF_INET, &in->sin_addr, buf, sizeof(buf)) == nullptr) return {};
        return buf;
    }
    if (addr.ss_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        if (::inet_ntop(AF_INET6, &in6->sin6_addr, buf, sizeof(buf)) == nullptr) return {};
        return buf;
    }
    return {};
}

Endpoint endpoint_of_fd(native_t fd, bool peer) {
    Endpoint ep;
    sockaddr_storage ss{};
    socklen_t        len = sizeof(ss);
    const int        rc  = peer ? ::getpeername(fd, reinterpret_cast<sockaddr*>(&ss), &len)
                                : ::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len);
    if (rc != 0) return ep;
    ep.host = ip_of(ss);
    ep.port = port_of(ss);
    return ep;
}

}  // namespace

// =============================================================================
//  Endpoint
// =============================================================================
std::string Endpoint::to_string() const {
    const bool v6 = host.find(':') != std::string::npos;
    if (v6) return "[" + host + "]:" + std::to_string(port);
    return host + ":" + std::to_string(port);
}

namespace {

/// 手写十进制解析：std::stol 会抛异常，而 net 组件要能进 -fno-exceptions 子集
Result<std::uint16_t> parse_port_text(std::string_view text) {
    if (text.empty()) return Status(Errc::InvalidArgument, "端口为空");
    unsigned value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return Status(Errc::ParseError, "端口不是数字: " + std::string(text));
        }
        value = value * 10u + static_cast<unsigned>(c - '0');
        if (value > 65535u) return Status(Errc::OutOfRange, "端口越界: " + std::string(text));
    }
    return static_cast<std::uint16_t>(value);
}

}  // namespace

Result<Endpoint> parse_endpoint(std::string_view text, std::uint16_t default_port) {
    if (text.empty()) return Status(Errc::InvalidArgument, "地址为空");
    std::string s(text);

    Endpoint ep;
    if (s.front() == '[') {  // [::1]:8080
        const auto close = s.find(']');
        if (close == std::string::npos) {
            return Status(Errc::InvalidArgument, "IPv6 地址缺少 ']': " + s);
        }
        ep.host = s.substr(1, close - 1);
        if (close + 1 < s.size()) {
            if (s[close + 1] != ':') return Status(Errc::InvalidArgument, "']' 后应为 ':': " + s);
            const std::string p = s.substr(close + 2);
            if (!p.empty()) {
                auto port = parse_port_text(p);
                if (!port) return port.status();
                ep.port = port.value();
            } else {
                ep.port = default_port;   // 与 "host:" 的行为保持一致
            }
        } else {
            ep.port = default_port;
        }
        return ep;
    }

    const auto colon = s.rfind(':');
    if (colon == std::string::npos) {  // 只有主机名
        ep.host = s;
        ep.port = default_port;
        return ep;
    }
    // 多个 ':' 且没方括号 → 可能是裸 IPv6，按"没有端口"处理
    if (s.find(':') != colon) {
        ep.host = s;
        ep.port = default_port;
        return ep;
    }

    ep.host = s.substr(0, colon);
    const std::string p = s.substr(colon + 1);
    if (p.empty()) {
        ep.port = default_port;
        return ep;
    }
    auto port = parse_port_text(p);
    if (!port) return port.status();
    ep.port = port.value();
    return ep;
}

// =============================================================================
//  TcpSocket
// =============================================================================
TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        fd_       = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

void TcpSocket::close() noexcept {
    if (fd_ >= 0) {
        close_fd(static_cast<native_t>(fd_));
        fd_ = -1;
    }
}

bool TcpSocket::valid() const noexcept { return fd_ != static_cast<std::intptr_t>(kInvalidFd); }

std::intptr_t TcpSocket::native_handle() const noexcept { return fd_; }

Result<TcpSocket> TcpSocket::connect(const Endpoint& ep, int timeout_ms) {
    ensure_wsa();
    if (ep.port == 0) return Status(Errc::InvalidArgument, "端口不能为 0");

    auto addrs = resolve(ep, /*passive=*/false, SOCK_STREAM);
    if (!addrs) return addrs.status();

    int last_err = 0;
    for (const auto& a : addrs.value()) {
        native_t fd = ::socket(a.family, SOCK_STREAM, 0);
        if (fd == kInvalidFd) {
            last_err = MYLIB_SOCK_ERRNO();
            continue;
        }
        no_sigpipe(fd);
        set_blocking(fd, false);
        int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&a.addr), a.len);
        bool ok = (rc == 0);

        if (!ok) {
            const int e = MYLIB_SOCK_ERRNO();
            if (MYLIB_IN_PROGRESS(e)) {
                const int w = wait_ready(fd, /*for_write=*/true, timeout_ms <= 0 ? -1 : timeout_ms);
                if (w == 0) {
                    close_fd(fd);
                    return Status(Errc::Timeout, "连接超时: " + ep.to_string());
                }
                if (w > 0) {
                    int       so_err = 0;
                    socklen_t l      = sizeof(so_err);
                    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err),
                                     &l) != 0) {
                        last_err = MYLIB_SOCK_ERRNO();
                        ok       = false;
                    } else {
                        ok = (so_err == 0);
                        if (!ok) last_err = so_err;
                    }
                } else {
                    last_err = MYLIB_SOCK_ERRNO();
                }
            } else {
                last_err = e;
            }
        }

        if (!ok) {
            close_fd(fd);
            continue;
        }

        set_blocking(fd, true);
        // 注意：这里的 timeout 只用于 connect，不顺手设成收发超时
        // （否则用户拿到 socket 后会在完全没预期的时刻超时）。
        return TcpSocket(static_cast<std::intptr_t>(fd));
    }

    return Status(Errc::IoError, "连接失败: " + ep.to_string() + "（" + errno_text(last_err) + "）");
}

Status TcpSocket::set_timeout(int ms) {
    if (!valid()) return Status(Errc::NotInitialized, "socket 未打开");
    if (ms < 0) ms = 0;
    const native_t fd = static_cast<native_t>(fd_);
#if defined(_WIN32)
    const DWORD v = static_cast<DWORD>(ms);
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&v), sizeof(v)) != 0 ||
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&v), sizeof(v)) != 0) {
        return sock_status("setsockopt(超时)");
    }
#else
    timeval tv{};
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        return sock_status("setsockopt(超时)");
    }
#endif
    return Ok();
}

Result<std::size_t> TcpSocket::send(const void* data, std::size_t size) {
    if (!valid()) return Status(Errc::NotInitialized, "socket 未打开");
    if (size == 0) return std::size_t{0};

    const native_t fd = static_cast<native_t>(fd_);
    for (;;) {
#if defined(_WIN32)
        const int chunk = static_cast<int>(std::min<std::size_t>(size, std::numeric_limits<int>::max()));
        const int rc    = ::send(fd, static_cast<const char*>(data), chunk, 0);
        if (rc == SOCKET_ERROR) {
            if (MYLIB_INTERRUPTED(MYLIB_SOCK_ERRNO())) continue;  // 被信号打断，重试
            return sock_status("send");
        }
        return static_cast<std::size_t>(rc);
#else
        const ssize_t rc = ::send(fd, data, size, MSG_NOSIGNAL);
        if (rc < 0) {
            if (MYLIB_INTERRUPTED(MYLIB_SOCK_ERRNO())) continue;  // 被信号打断，重试
            return sock_status("send");
        }
        return static_cast<std::size_t>(rc);
#endif
    }
}

Status TcpSocket::send_all(const void* data, std::size_t size) {
    const auto* p     = static_cast<const std::uint8_t*>(data);
    std::size_t sent  = 0;
    int         retry = 0;
    while (sent < size) {
        auto r = send(p + sent, size - sent);
        if (!r) return r.status();
        if (r.value() == 0) {
            if (++retry > 1000) return Status(Errc::IoError, "send_all: 连续空写");
            continue;
        }
        retry = 0;
        sent += r.value();
    }
    return Ok();
}

Result<std::size_t> TcpSocket::recv(void* buffer, std::size_t size) {
    if (!valid()) return Status(Errc::NotInitialized, "socket 未打开");
    if (size == 0) return std::size_t{0};

    const native_t fd = static_cast<native_t>(fd_);
    for (;;) {
#if defined(_WIN32)
        const int chunk = static_cast<int>(std::min<std::size_t>(size, std::numeric_limits<int>::max()));
        const int rc    = ::recv(fd, static_cast<char*>(buffer), chunk, 0);
        if (rc == SOCKET_ERROR) {
            if (MYLIB_INTERRUPTED(MYLIB_SOCK_ERRNO())) continue;  // 被信号打断，重试
            return sock_status("recv");
        }
        return static_cast<std::size_t>(rc);   // 0 = 对端关闭
#else
        const ssize_t rc = ::recv(fd, buffer, size, 0);
        if (rc < 0) {
            if (MYLIB_INTERRUPTED(MYLIB_SOCK_ERRNO())) continue;  // 被信号打断，重试
            return sock_status("recv");
        }
        return static_cast<std::size_t>(rc);   // 0 = 对端关闭
#endif
    }
}

Result<Bytes> TcpSocket::recv_exact(std::size_t size) {
    // 顺序：先做纯参数校验（不碰状态、不分配），再查 socket 有效性，最后才分配。
    //   * 参数越界属于调用方错误，和 socket 有没有连上无关，先报；
    //   * 有效性必须在 Bytes out(size) 之前查，否则"未连接的 socket + 256 MiB 请求"
    //     会先吃掉 256 MiB 再返回 NotInitialized（小内存板子上可能直接败）。
    if (size > kMaxBufferBytes) {
        return Status(Errc::OutOfRange, "recv_exact 请求的字节数过大: " + std::to_string(size) +
                                            "（上限 " + std::to_string(kMaxBufferBytes) + "）");
    }
    if (!valid()) return Status(Errc::NotInitialized, "socket 未打开");
    Bytes out(size);
    std::size_t got = 0;
    while (got < size) {
        auto r = recv(out.data() + got, size - got);
        if (!r) {
            if (r.status().code() == Errc::Timeout) return r.status();
            return Status(r.status().code(),
                          "recv_exact 失败（已收 " + std::to_string(got) + "/" +
                              std::to_string(size) + "）: " + r.status().message());
        }
        if (r.value() == 0) {
            return Status(Errc::IoError,
                          "对端关闭连接（已收 " + std::to_string(got) + "/" +
                              std::to_string(size) + " 字节）");
        }
        got += r.value();
    }
    return out;
}

Status TcpSocket::shutdown_write() {
    if (!valid()) return Status(Errc::NotInitialized, "socket 未打开");
    const native_t fd = static_cast<native_t>(fd_);
    if (::shutdown(fd, kShutWr) != 0) return sock_status("shutdown");
    return Ok();
}

Endpoint TcpSocket::peer() const {
    if (!valid()) return {};
    return endpoint_of_fd(static_cast<native_t>(fd_), /*peer=*/true);
}

Endpoint TcpSocket::local() const {
    if (!valid()) return {};
    return endpoint_of_fd(static_cast<native_t>(fd_), /*peer=*/false);
}

// =============================================================================
//  TcpListener
// =============================================================================
TcpListener::~TcpListener() { close(); }

TcpListener::TcpListener(TcpListener&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        close();
        fd_       = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

void TcpListener::close() noexcept {
    if (fd_ >= 0) {
        close_fd(static_cast<native_t>(fd_));
        fd_ = -1;
    }
}

bool TcpListener::valid() const noexcept { return fd_ >= 0; }

Result<TcpListener> TcpListener::bind(const Endpoint& ep, int backlog, bool reuse_addr) {
    ensure_wsa();

    auto addrs = resolve(ep, /*passive=*/true, SOCK_STREAM);
    if (!addrs) return addrs.status();

    int last_err = 0;
    for (const auto& a : addrs.value()) {
        native_t fd = ::socket(a.family, SOCK_STREAM, 0);
        if (fd == kInvalidFd) {
            last_err = MYLIB_SOCK_ERRNO();
            continue;
        }
        no_sigpipe(fd);
        if (reuse_addr) {
            int one = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
                         sizeof(one));
        }
        if (::bind(fd, reinterpret_cast<const sockaddr*>(&a.addr), a.len) != 0) {
            last_err = MYLIB_SOCK_ERRNO();
            close_fd(fd);
            continue;
        }
        if (::listen(fd, backlog) != 0) {
            last_err = MYLIB_SOCK_ERRNO();
            close_fd(fd);
            continue;
        }
        TcpListener l;
        l.fd_ = static_cast<std::intptr_t>(fd);
        return l;
    }
    return Status(Errc::IoError, "bind 失败: " + ep.to_string() + "（" + errno_text(last_err) + "）");
}

Result<TcpSocket> TcpListener::accept(int timeout_ms) {
    if (!valid()) return Status(Errc::NotInitialized, "listener 未打开");
    const native_t fd = static_cast<native_t>(fd_);

    if (timeout_ms >= 0) {
        const int w = wait_ready(fd, /*for_write=*/false, timeout_ms);
        if (w == 0) return Status(Errc::Timeout, "accept 超时（" + std::to_string(timeout_ms) + " ms）");
        if (w < 0) return sock_status("select(accept)");
    }

    sockaddr_storage ss{};
    socklen_t        len = sizeof(ss);
    for (;;) {
        const native_t c = ::accept(fd, reinterpret_cast<sockaddr*>(&ss), &len);
        if (c == kInvalidFd) {
            if (MYLIB_INTERRUPTED(MYLIB_SOCK_ERRNO())) continue;  // 被信号打断，重试
            return sock_status("accept");
        }
        return TcpSocket(static_cast<std::intptr_t>(c));
    }
}

std::uint16_t TcpListener::port() const {
    if (!valid()) return 0;
    return endpoint_of_fd(static_cast<native_t>(fd_), /*peer=*/false).port;
}

// =============================================================================
//  UdpSocket
// =============================================================================
UdpSocket::~UdpSocket() { close(); }

UdpSocket::UdpSocket(UdpSocket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        fd_       = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

void UdpSocket::close() noexcept {
    if (fd_ >= 0) {
        close_fd(static_cast<native_t>(fd_));
        fd_ = -1;
    }
}

bool UdpSocket::valid() const noexcept { return fd_ >= 0; }

Result<UdpSocket> UdpSocket::bind(const Endpoint& ep) {
    ensure_wsa();
    auto addrs = resolve(ep, /*passive=*/true, SOCK_DGRAM);
    if (!addrs) return addrs.status();

    int last_err = 0;
    for (const auto& a : addrs.value()) {
        native_t fd = ::socket(a.family, SOCK_DGRAM, 0);
        if (fd == kInvalidFd) {
            last_err = MYLIB_SOCK_ERRNO();
            continue;
        }
        if (::bind(fd, reinterpret_cast<const sockaddr*>(&a.addr), a.len) != 0) {
            last_err = MYLIB_SOCK_ERRNO();
            close_fd(fd);
            continue;
        }
        UdpSocket s;
        s.fd_ = static_cast<std::intptr_t>(fd);
        return s;
    }
    return Status(Errc::IoError, "UDP bind 失败: " + ep.to_string() + "（" + errno_text(last_err) + "）");
}

Result<UdpSocket> UdpSocket::open() {
    ensure_wsa();
    native_t fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == kInvalidFd) return sock_status("socket(UDP)");
    UdpSocket s;
    s.fd_ = static_cast<std::intptr_t>(fd);
    return s;
}

Status UdpSocket::set_timeout(int ms) {
    if (!valid()) return Status(Errc::NotInitialized, "socket 未打开");
    if (ms < 0) ms = 0;
    const native_t fd = static_cast<native_t>(fd_);
#if defined(_WIN32)
    const DWORD v = static_cast<DWORD>(ms);
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&v), sizeof(v)) != 0 ||
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&v), sizeof(v)) != 0) {
        return sock_status("setsockopt(超时)");
    }
#else
    timeval tv{};
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        return sock_status("setsockopt(超时)");
    }
#endif
    return Ok();
}

Status UdpSocket::set_broadcast(bool on) {
    if (!valid()) return Status(Errc::NotInitialized, "socket 未打开");
    const int      one = on ? 1 : 0;
    const native_t fd  = static_cast<native_t>(fd_);
    if (::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one),
                     sizeof(one)) != 0) {
        return sock_status("setsockopt(SO_BROADCAST)");
    }
    return Ok();
}

Status UdpSocket::join_multicast(const std::string& group, const std::string& local_iface) {
    if (!valid()) return Status(Errc::NotInitialized, "socket 未打开");
    const native_t fd = static_cast<native_t>(fd_);

    in_addr g{};
    if (::inet_pton(AF_INET, group.c_str(), &g) != 1) {
        return Status(Errc::InvalidArgument, "组播地址非法（只支持 IPv4）: " + group);
    }
    ip_mreq mreq{};
    mreq.imr_multiaddr = g;
    if (local_iface.empty()) {
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, local_iface.c_str(), &mreq.imr_interface) != 1) {
        return Status(Errc::InvalidArgument, "网卡地址非法: " + local_iface);
    }
    if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq),
                     sizeof(mreq)) != 0) {
        return sock_status("加入组播组");
    }
    return Ok();
}

Status UdpSocket::leave_multicast(const std::string& group, const std::string& local_iface) {
    if (!valid()) return Status(Errc::NotInitialized, "socket 未打开");
    const native_t fd = static_cast<native_t>(fd_);

    in_addr g{};
    if (::inet_pton(AF_INET, group.c_str(), &g) != 1) {
        return Status(Errc::InvalidArgument, "组播地址非法（只支持 IPv4）: " + group);
    }
    ip_mreq mreq{};
    mreq.imr_multiaddr = g;
    if (local_iface.empty()) {
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, local_iface.c_str(), &mreq.imr_interface) != 1) {
        return Status(Errc::InvalidArgument, "网卡地址非法: " + local_iface);
    }
    if (::setsockopt(fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, reinterpret_cast<const char*>(&mreq),
                     sizeof(mreq)) != 0) {
        return sock_status("退出组播组");
    }
    return Ok();
}

Result<std::size_t> UdpSocket::send_to(const Endpoint& ep, const void* data, std::size_t size) {
    if (!valid()) return Status(Errc::NotInitialized, "socket 未打开");
    if (ep.port == 0) return Status(Errc::InvalidArgument, "目标端口不能为 0");

    auto addrs = resolve(ep, /*passive=*/false, SOCK_DGRAM);
    if (!addrs) return addrs.status();

    const native_t fd = static_cast<native_t>(fd_);
    for (const auto& a : addrs.value()) {
#if defined(_WIN32)
        const int rc = ::sendto(fd, static_cast<const char*>(data),
                                static_cast<int>(std::min<std::size_t>(
                                    size, std::numeric_limits<int>::max())),
                                0, reinterpret_cast<const sockaddr*>(&a.addr), a.len);
        if (rc == SOCKET_ERROR) return sock_status("sendto");
        return static_cast<std::size_t>(rc);
#else
        const ssize_t rc = ::sendto(fd, data, size, 0, reinterpret_cast<const sockaddr*>(&a.addr),
                                    a.len);
        if (rc < 0) {
            const int e = MYLIB_SOCK_ERRNO();
            if (MYLIB_INTERRUPTED(e)) continue;
            return sock_status("sendto");
        }
        return static_cast<std::size_t>(rc);
#endif
    }
    return Status(Errc::IoError, "sendto 没有可用地址: " + ep.to_string());
}

Result<Datagram> UdpSocket::recv_from(std::size_t max_size) {
    if (!valid()) return Status(Errc::NotInitialized, "socket 未打开");
    if (max_size == 0) return Status(Errc::InvalidArgument, "max_size 不能为 0");

    Datagram         d;
    sockaddr_storage ss{};
    socklen_t        len = sizeof(ss);
    const native_t   fd  = static_cast<native_t>(fd_);
    d.data.resize(max_size);

    for (;;) {
#if defined(_WIN32)
        const int rc = ::recvfrom(fd, reinterpret_cast<char*>(d.data.data()),
                                  static_cast<int>(max_size), 0,
                                  reinterpret_cast<sockaddr*>(&ss), &len);
        if (rc == SOCKET_ERROR) {
            if (MYLIB_INTERRUPTED(MYLIB_SOCK_ERRNO())) continue;
            return sock_status("recvfrom");
        }
#else
        len = sizeof(ss);  // recvfrom 会改写 len，重试前恢复
        const ssize_t rc = ::recvfrom(fd, d.data.data(), max_size, 0,
                                      reinterpret_cast<sockaddr*>(&ss), &len);
        if (rc < 0) {
            if (MYLIB_INTERRUPTED(MYLIB_SOCK_ERRNO())) continue;
            return sock_status("recvfrom");
        }
#endif
        d.data.resize(static_cast<std::size_t>(rc));
        d.from.host = ip_of(ss);
        d.from.port = port_of(ss);
        return d;
    }
}

std::uint16_t UdpSocket::port() const {
    if (!valid()) return 0;
    return endpoint_of_fd(static_cast<native_t>(fd_), /*peer=*/false).port;
}

// =============================================================================
//  分帧
// =============================================================================
Status read_line(TcpSocket& sock, std::string& line, std::size_t max_len) {
    line.clear();
    line.reserve(64);
    char c = 0;
    while (true) {
        auto r = sock.recv(&c, 1);
        if (!r) return r.status();
        if (r.value() == 0) {
            if (line.empty()) return Status(Errc::IoError, "对端关闭连接（未读到行）");
            return Ok();  // 没有换行的最后一行，也算读到
        }
        if (c == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return Ok();
        }
        line.push_back(c);
        if (line.size() >= max_len) {
            return Status(Errc::OutOfRange,
                          "行长度超过上限 " + std::to_string(max_len) + "（对端可能不是行协议）");
        }
    }
}

Status write_frame(TcpSocket& sock, const void* data, std::size_t size) {
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        return Status(Errc::OutOfRange, "帧太大: " + std::to_string(size));
    }
    std::uint8_t head[4];
    const std::uint32_t n = static_cast<std::uint32_t>(size);
    head[0] = static_cast<std::uint8_t>((n >> 24) & 0xFF);
    head[1] = static_cast<std::uint8_t>((n >> 16) & 0xFF);
    head[2] = static_cast<std::uint8_t>((n >> 8) & 0xFF);
    head[3] = static_cast<std::uint8_t>(n & 0xFF);

    if (auto st = sock.send_all(head, 4); !st) return st;
    if (size == 0) return Ok();
    return sock.send_all(data, size);
}

Status write_frame(TcpSocket& sock, const std::string& text) {
    return write_frame(sock, text.data(), text.size());
}

Result<Bytes> read_frame(TcpSocket& sock, std::size_t max_size) {
    auto head = sock.recv_exact(4);
    if (!head) return head.status();

    const std::uint32_t n = (static_cast<std::uint32_t>(head.value()[0]) << 24) |
                            (static_cast<std::uint32_t>(head.value()[1]) << 16) |
                            (static_cast<std::uint32_t>(head.value()[2]) << 8) |
                            static_cast<std::uint32_t>(head.value()[3]);
    if (n > max_size) {
        return Status(Errc::OutOfRange,
                      "帧长度 " + std::to_string(n) + " 超过上限 " + std::to_string(max_size));
    }
    return sock.recv_exact(n);
}

}  // namespace my::net
