// =============================================================================
//  my::serial 实现 —— Windows（COM）与 POSIX（termios）两条腿
//
//  平台差异集中在这里：
//    * 打开：CreateFile("\\\\.\\COMx")            / open(O_RDWR|O_NOCTTY|O_NONBLOCK)
//    * 配置：DCB + SetCommState                  / termios + tcsetattr(TCSANOW)
//    * 超时：每次调用设 COMMTIMEOUTS + ReadFile  / poll() 等可读可写
//    * 可读字节数：ClearCommError→cbInQue        / ioctl(FIONREAD)
//    * 清缓冲：PurgeComm                         / tcflush(TCIOFLUSH)
//    * 枚举：注册表 SERIALCOMM                   / 扫 /dev/tty*
//
//  错误一律翻译成 my::Status，消息里带 errno / GetLastError —— 板端排查时这行消息
//  比一个枚举值有用得多。
// =============================================================================

#include "my_cpp_libs/serial/serial.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <system_error>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <fcntl.h>
#    include <poll.h>
#    include <sys/ioctl.h>
#    include <termios.h>
#    include <unistd.h>

#    include <filesystem>
#    if defined(__linux__)
#        include <sys/ioctl.h>  // TIOCEXCL / FIONREAD
#    endif
#endif

namespace my::serial {
namespace {

// -----------------------------------------------------------------------------
//  平台小工具
// -----------------------------------------------------------------------------
#if defined(_WIN32)

std::intptr_t to_handle(HANDLE h) noexcept {
    return reinterpret_cast<std::intptr_t>(h);
}
HANDLE from_handle(std::intptr_t h) noexcept {
    return reinterpret_cast<HANDLE>(h);
}

/// 把 "COM3" / "COM3:" / "\\\\.\\COM3" 统一成 CreateFile 需要的形态。
std::string normalize_windows_name(std::string_view name) {
    std::string s(name);
    if (s.size() > 1 && s.back() == ':') s.pop_back();
    if (s.rfind("\\\\.\\", 0) == 0) return s;  // 已经带前缀
    // COM10 及以上**必须**带前缀，否则 CreateFile 直接失败；
    // 统一都加，低编号加了也无害。
    return "\\\\.\\" + s;
}

Status io_status(const char* what, const std::string& context = {}) {
    const DWORD err = ::GetLastError();
    std::string msg(what);
    if (!context.empty()) {
        msg += " " + context;
    }
    msg += " 失败 (GetLastError=" + std::to_string(err) + ")";
    return Status(Errc::IoError, std::move(msg));
}

#else  //  POSIX

using HANDLE = int;

std::intptr_t to_handle(int fd) noexcept { return static_cast<std::intptr_t>(fd); }
int           from_handle(std::intptr_t h) noexcept { return static_cast<int>(h); }

Status io_status(const char* what, const std::string& context = {}) {
    const int     err = errno;
    std::string   msg(what);
    if (!context.empty()) {
        msg += " " + context;
    }
    msg += " 失败: ";
    msg += std::strerror(err);
    msg += " (errno=" + std::to_string(err) + ")";
    return Status(Errc::IoError, std::move(msg));
}

#endif

/// chrono::milliseconds → poll 的 int（截断到 int 范围；负值表示无限等待）
int to_poll_ms(std::chrono::milliseconds timeout) noexcept {
    if (timeout.count() < 0) return -1;
    const auto c = timeout.count();
    if (c > static_cast<long long>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(c);
}

}  // namespace

// -----------------------------------------------------------------------------
//  波特率表（Linux）
// -----------------------------------------------------------------------------
const std::vector<std::uint32_t>& standard_bauds() {
#if defined(_WIN32)
    // Windows 的 DCB.BaudRate 接受任意值，没有"标准档位"这个概念。
    static const std::vector<std::uint32_t> kEmpty{};
    return kEmpty;
#else
    static const std::vector<std::uint32_t> kBauds = {
        50,    75,    110,   134,   150,   200,   300,   600,   1200,  1800,
        2400,  4800,  9600,  19200, 38400, 57600, 115200, 230400, 460800, 500000,
        576000, 921600, 1000000, 1152000, 1500000, 2000000, 2500000, 3000000, 3500000, 4000000,
    };
    return kBauds;
#endif
}

bool is_baud_supported(std::uint32_t baud) {
#if defined(_WIN32)
    (void)baud;
    return true;  // 任意值的 DCB 都可能成功，交给 SetCommState 判断
#else
    const auto& v = standard_bauds();
    return std::find(v.begin(), v.end(), baud) != v.end();
#endif
}

#if !defined(_WIN32)
namespace {

/// 数值波特率 → termios 的 speed_t。不支持返回 false。
bool baud_to_speed(std::uint32_t baud, speed_t& out) {
    struct Entry {
        std::uint32_t value;
        speed_t       code;
    };
    // 用 #ifdef 逐个保护：不同 libc/内核头文件暴露的档位不一样
    // （B576000 以上是 Linux 专有，POSIX/macOS 没有）。
    static const Entry kTable[] = {
#ifdef B50
        {50, B50},
#endif
#ifdef B75
        {75, B75},
#endif
#ifdef B110
        {110, B110},
#endif
#ifdef B134
        {134, B134},
#endif
#ifdef B150
        {150, B150},
#endif
#ifdef B200
        {200, B200},
#endif
#ifdef B300
        {300, B300},
#endif
#ifdef B600
        {600, B600},
#endif
#ifdef B1200
        {1200, B1200},
#endif
#ifdef B1800
        {1800, B1800},
#endif
#ifdef B2400
        {2400, B2400},
#endif
#ifdef B4800
        {4800, B4800},
#endif
#ifdef B9600
        {9600, B9600},
#endif
#ifdef B19200
        {19200, B19200},
#endif
#ifdef B38400
        {38400, B38400},
#endif
#ifdef B57600
        {57600, B57600},
#endif
#ifdef B115200
        {115200, B115200},
#endif
#ifdef B230400
        {230400, B230400},
#endif
#ifdef B460800
        {460800, B460800},
#endif
#ifdef B500000
        {500000, B500000},
#endif
#ifdef B576000
        {576000, B576000},
#endif
#ifdef B921600
        {921600, B921600},
#endif
#ifdef B1000000
        {1000000, B1000000},
#endif
#ifdef B1152000
        {1152000, B1152000},
#endif
#ifdef B1500000
        {1500000, B1500000},
#endif
#ifdef B2000000
        {2000000, B2000000},
#endif
#ifdef B2500000
        {2500000, B2500000},
#endif
#ifdef B3000000
        {3000000, B3000000},
#endif
#ifdef B3500000
        {3500000, B3500000},
#endif
#ifdef B4000000
        {4000000, B4000000},
#endif
    };
    for (const auto& e : kTable) {
        if (e.value == baud) {
            out = e.code;
            return true;
        }
    }
    return false;
}

}  // namespace
#endif  // !_WIN32

// -----------------------------------------------------------------------------
//  参数校验
// -----------------------------------------------------------------------------
Status validate(const Config& cfg) {
    if (cfg.baud == 0) {
        return Status(Errc::InvalidArgument, "波特率不能为 0");
    }
    if (cfg.data_bits < 5 || cfg.data_bits > 8) {
        return Status(Errc::InvalidArgument,
                      "data_bits 必须在 5..8 之间（收到 " + std::to_string(cfg.data_bits) + "）");
    }
    if (!is_baud_supported(cfg.baud)) {
        std::string supported;
        for (auto b : standard_bauds()) {
            if (!supported.empty()) supported += ", ";
            supported += std::to_string(b);
        }
        return Status(Errc::InvalidArgument,
                      "本平台不支持波特率 " + std::to_string(cfg.baud) +
                          "；termios 只支持标准档位：" + supported);
    }
    return Status{};
}

// -----------------------------------------------------------------------------
//  Port：生命周期
// -----------------------------------------------------------------------------
Port::~Port() { (void)close(); }

Port::Port(Port&& other) noexcept : handle_(other.handle_), name_(std::move(other.name_)) {
    other.handle_ = -1;
    other.name_.clear();
}

Port& Port::operator=(Port&& other) noexcept {
    if (this != &other) {
        (void)close();
        handle_ = other.handle_;
        name_   = std::move(other.name_);
        other.handle_ = -1;
        other.name_.clear();
    }
    return *this;
}

Status Port::close() noexcept {
    if (handle_ < 0) return Status{};
#if defined(_WIN32)
    ::CloseHandle(from_handle(handle_));
#else
    ::close(from_handle(handle_));
#endif
    handle_ = -1;
    name_.clear();
    return Status{};
}

// -----------------------------------------------------------------------------
//  Port::open
// -----------------------------------------------------------------------------
Result<Port> Port::open(std::string_view name, const Config& cfg) {
    if (name.empty()) {
        return Status(Errc::InvalidArgument, "端口名为空");
    }
    // 先校验参数：这样"设备不存在 + 参数非法"时报的是参数问题（更能说明是调用方写错了）
    if (Status st = validate(cfg); !st.ok()) return st;

    Port p;
#if defined(_WIN32)
    const std::string path = normalize_windows_name(name);
    // dwShareMode = 0：独占。Windows 上串口被占用时 CreateFile 会返回 ERROR_ACCESS_DENIED，
    // 这里把这种情况翻译成 Busy，比笼统的 IoError 更有用。
    HANDLE h = ::CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                             0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            return Status(Errc::Busy, "串口被占用（可能被别的进程打开）: " + std::string(name));
        }
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return Status(Errc::NotFound, "串口不存在: " + std::string(name));
        }
        return io_status("CreateFile", std::string(name));
    }
    p.handle_ = to_handle(h);
    p.name_.assign(name);

    if (!::SetupComm(h, 8192, 8192)) {
        // 设置缓冲失败不致命，继续（下面 SetCommState 才是必须成功的）
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!::GetCommState(h, &dcb)) {
        return io_status("GetCommState", std::string(name));
    }
    dcb.BaudRate  = cfg.baud;
    dcb.ByteSize  = cfg.data_bits;
    dcb.fBinary   = TRUE;
    dcb.fParity   = (cfg.parity != Parity::None) ? TRUE : FALSE;
    dcb.Parity    = (cfg.parity == Parity::Even) ? EVENPARITY
                    : (cfg.parity == Parity::Odd) ? ODDPARITY
                                                  : NOPARITY;
    dcb.StopBits  = (cfg.stop_bits == StopBits::Two) ? TWOSTOPBITS : ONESTOPBIT;
    dcb.fOutxCtsFlow = (cfg.flow == Flow::Hardware) ? TRUE : FALSE;
    dcb.fRtsControl  = (cfg.flow == Flow::Hardware) ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_ENABLE;
    dcb.fOutX        = (cfg.flow == Flow::Software) ? TRUE : FALSE;
    dcb.fInX         = (cfg.flow == Flow::Software) ? TRUE : FALSE;
    if (!::SetCommState(h, &dcb)) {
        return io_status("SetCommState", std::string(name));
    }
    // 清掉上次残留的数据，避免"打开就收到一坨旧数据"
    ::PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
#else
    const std::string path(name);
    const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        const int err = errno;
        if (err == ENOENT || err == ENODEV) {
            return Status(Errc::NotFound, "串口不存在: " + path);
        }
        if (err == EACCES || err == EPERM) {
            // 板端最常见：用户不在 dialout 组
            return Status(Errc::IoError, "没有权限打开 " + path +
                                             "（检查用户是否在 dialout 组，或设备的属主/权限）");
        }
        if (err == EBUSY) {
            return Status(Errc::Busy, "串口被占用: " + path);
        }
        errno = err;
        return io_status("open", path);
    }
    p.handle_ = to_handle(fd);
    p.name_.assign(name);

    // 独占：别的进程再 open 会拿到 EBUSY。这个只在 Linux 上有效（macOS 无 TIOCEXCL）。
#    if defined(TIOCEXCL) && defined(__linux__)
    if (cfg.exclusive) {
        if (::ioctl(fd, TIOCEXCL) < 0) {
            // 不致命：仍然可以工作，只是不是独占
        }
    }
#    else
    (void)cfg.exclusive;
#    endif

    termios tio{};
    if (::tcgetattr(fd, &tio) < 0) {
        return io_status("tcgetattr", path);
    }

    speed_t speed = B0;
    if (!baud_to_speed(cfg.baud, speed)) {
        // validate() 已经挡过一次，这里是防御（比如换了 libc）
        return Status(Errc::InvalidArgument, "本平台不支持波特率 " + std::to_string(cfg.baud));
    }

    // 原始模式：不做任何字符翻译/回显/信号（板端收的是二进制帧，不是终端会话）
    tio.c_iflag &= static_cast<tcflag_t>(~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR |
                                           ICRNL | IXON | IXOFF | IXANY));
    tio.c_oflag &= static_cast<tcflag_t>(~OPOST);
    tio.c_lflag &= static_cast<tcflag_t>(~(ECHO | ECHONL | ICANON | ISIG | IEXTEN));
    tio.c_cflag &= static_cast<tcflag_t>(~(CSIZE | PARENB | CSTOPB));
    tio.c_cflag |= CS8 | CREAD | CLOCAL;
    tio.c_cflag &= static_cast<tcflag_t>(~CSIZE);

    switch (cfg.data_bits) {
        case 5: tio.c_cflag |= CS5; break;
        case 6: tio.c_cflag |= CS6; break;
        case 7: tio.c_cflag |= CS7; break;
        default: tio.c_cflag |= CS8; break;
    }
    if (cfg.parity == Parity::Even) {
        tio.c_cflag |= PARENB;
        tio.c_cflag &= static_cast<tcflag_t>(~PARODD);
        tio.c_iflag |= INPCK;
    } else if (cfg.parity == Parity::Odd) {
        tio.c_cflag |= PARENB | PARODD;
        tio.c_iflag |= INPCK;
    }
    if (cfg.stop_bits == StopBits::Two) {
        tio.c_cflag |= CSTOPB;
    }
#    if defined(CRTSCTS)
    if (cfg.flow == Flow::Hardware) {
        tio.c_cflag |= CRTSCTS;
    } else {
        tio.c_cflag &= static_cast<tcflag_t>(~CRTSCTS);
    }
#    endif
    if (cfg.flow == Flow::Software) {
        tio.c_iflag |= IXON | IXOFF;
    }
    // 非阻塞 fd 上用 poll 等超时，这两个字段不起作用，置 0 让语义明确
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    if (::cfsetispeed(&tio, speed) < 0 || ::cfsetospeed(&tio, speed) < 0) {
        return io_status("cfsetispeed/cfsetospeed", path);
    }
    if (::tcsetattr(fd, TCSANOW, &tio) < 0) {
        return io_status("tcsetattr", path);
    }
    ::tcflush(fd, TCIOFLUSH);
#endif
    return p;
}

// -----------------------------------------------------------------------------
//  读
// -----------------------------------------------------------------------------
Result<std::size_t> Port::read_some(void* buf, std::size_t n, std::chrono::milliseconds timeout) {
    if (handle_ < 0) return Status(Errc::NotInitialized, "串口未打开");
    if (n == 0) return std::size_t{0};
    if (buf == nullptr) return Status(Errc::InvalidArgument, "read_some: buf 为空但 n > 0");

#if defined(_WIN32)
    HANDLE h = from_handle(handle_);
    COMMTIMEOUTS to{};
    // ReadIntervalTimeout = MAXDWORD + TotalTimeoutConstant = ms：
    // ReadFile 立刻返回已缓冲的数据，否则最多等 ms。
    to.ReadIntervalTimeout         = MAXDWORD;
    to.ReadTotalTimeoutMultiplier  = 0;
    to.ReadTotalTimeoutConstant    = static_cast<DWORD>(to_poll_ms(timeout) < 0 ? 0 : to_poll_ms(timeout));
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant   = 0;
    if (!::SetCommTimeouts(h, &to)) return io_status("SetCommTimeouts", name_);

    DWORD got = 0;
    if (!::ReadFile(h, buf, static_cast<DWORD>(n), &got, nullptr)) {
        return io_status("ReadFile", name_);
    }
    return static_cast<std::size_t>(got);
#else
    const int fd = from_handle(handle_);
    const int ms = to_poll_ms(timeout);
    for (;;) {
        pollfd pfd{};
        pfd.fd     = fd;
        pfd.events = POLLIN;
        const int rc = ::poll(&pfd, 1, ms);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return io_status("poll", name_);
        }
        if (rc == 0) return std::size_t{0};  // 超时：没数据，不是错误
        break;
    }
    for (;;) {
        const ssize_t got = ::read(fd, buf, n);
        if (got < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return std::size_t{0};
            return io_status("read", name_);
        }
        return static_cast<std::size_t>(got);
    }
#endif
}

// -----------------------------------------------------------------------------
//  写
// -----------------------------------------------------------------------------
Result<std::size_t> Port::write_some(const void* buf, std::size_t n,
                                     std::chrono::milliseconds timeout) {
    if (handle_ < 0) return Status(Errc::NotInitialized, "串口未打开");
    if (n == 0) return std::size_t{0};
    if (buf == nullptr) return Status(Errc::InvalidArgument, "write_some: buf 为空但 n > 0");

#if defined(_WIN32)
    HANDLE h = from_handle(handle_);
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout         = MAXDWORD;
    to.ReadTotalTimeoutMultiplier  = 0;
    to.ReadTotalTimeoutConstant    = 0;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant   = static_cast<DWORD>(to_poll_ms(timeout) < 0 ? 0 : to_poll_ms(timeout));
    if (!::SetCommTimeouts(h, &to)) return io_status("SetCommTimeouts", name_);

    DWORD put = 0;
    if (!::WriteFile(h, buf, static_cast<DWORD>(n), &put, nullptr)) {
        return io_status("WriteFile", name_);
    }
    return static_cast<std::size_t>(put);
#else
    const int fd = from_handle(handle_);
    const int ms = to_poll_ms(timeout);
    for (;;) {
        pollfd pfd{};
        pfd.fd     = fd;
        pfd.events = POLLOUT;
        const int rc = ::poll(&pfd, 1, ms);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return io_status("poll(POLLOUT)", name_);
        }
        if (rc == 0) return std::size_t{0};  // 超时：一个字节也没写出去
        break;
    }
    for (;;) {
        const ssize_t put = ::write(fd, buf, n);
        if (put < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return std::size_t{0};
            return io_status("write", name_);
        }
        return static_cast<std::size_t>(put);
    }
#endif
}

Status Port::write_all(const void* buf, std::size_t n, std::chrono::milliseconds timeout) {
    if (n == 0) return Status{};
    const auto* p = static_cast<const std::uint8_t*>(buf);
    std::size_t done = 0;
    while (done < n) {
        auto r = write_some(p + done, n - done, timeout);
        if (!r) return r.status();
        if (r.value() == 0) {
            return Status(Errc::Timeout,
                          "写超时: 已写 " + std::to_string(done) + "/" + std::to_string(n) + " 字节");
        }
        done += r.value();
    }
    return Status{};
}

// -----------------------------------------------------------------------------
//  行读取
// -----------------------------------------------------------------------------
Result<std::string> Port::read_line(char delim, std::size_t max_len,
                                    std::chrono::milliseconds timeout) {
    if (max_len == 0) return std::string{};
    std::string out;
    out.reserve(std::min<std::size_t>(max_len, 64));

    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + timeout;

    while (out.size() < max_len) {
        char c = 0;
        auto n = read_some(&c, 1, std::chrono::milliseconds(0));  // 先看有没有已到达的数据
        if (!n) return n.status();
        if (n.value() == 0) {
            // 没数据：按剩余时间等，等不到就返回已读部分（可能为空）
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now());
            if (left.count() <= 0) break;
            auto waited = read_some(&c, 1, left);
            if (!waited) return waited.status();
            if (waited.value() == 0) break;  // 超时
        }
        if (c == delim) return out;
        out.push_back(c);
    }
    return out;
}

// -----------------------------------------------------------------------------
//  状态查询 / 清缓冲
// -----------------------------------------------------------------------------
Result<std::size_t> Port::bytes_available() {
    if (handle_ < 0) return Status(Errc::NotInitialized, "串口未打开");
#if defined(_WIN32)
    COMSTAT stat{};
    DWORD   errors = 0;
    if (!::ClearCommError(from_handle(handle_), &errors, &stat)) {
        return io_status("ClearCommError", name_);
    }
    return static_cast<std::size_t>(stat.cbInQue);
#else
    int pending = 0;
    if (::ioctl(from_handle(handle_), FIONREAD, &pending) < 0) {
        return io_status("ioctl(FIONREAD)", name_);
    }
    return static_cast<std::size_t>(pending < 0 ? 0 : pending);
#endif
}

Status Port::flush() noexcept {
    if (handle_ < 0) return Status{};
#if defined(_WIN32)
    if (!::PurgeComm(from_handle(handle_), PURGE_RXCLEAR | PURGE_TXCLEAR)) {
        return io_status("PurgeComm", name_);
    }
#else
    if (::tcflush(from_handle(handle_), TCIOFLUSH) < 0) {
        return io_status("tcflush", name_);
    }
#endif
    return Status{};
}

// -----------------------------------------------------------------------------
//  端口枚举
// -----------------------------------------------------------------------------
Result<std::vector<PortInfo>> list_ports() {
    std::vector<PortInfo> out;
#if defined(_WIN32)
    HKEY key = nullptr;
    const LONG rc = ::RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0,
                                    KEY_READ, &key);
    if (rc != ERROR_SUCCESS) {
        // 一个串口都没有时这个键可能不存在 —— 返回空表而不是错误
        return out;
    }
    struct KeyGuard {
        HKEY k;
        ~KeyGuard() {
            if (k) ::RegCloseKey(k);
        }
    } guard{key};

    for (DWORD i = 0;; ++i) {
        char  value_name[256] = {};
        DWORD name_len        = sizeof(value_name);
        BYTE  data[256]       = {};
        DWORD data_len        = sizeof(data);
        DWORD type            = 0;
        const LONG r = ::RegEnumValueA(key, i, value_name, &name_len, nullptr, &type, data, &data_len);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) break;
        if (type != REG_SZ || data_len == 0) continue;
        // data 是 "COM3" 这样的串，可能不保证 NUL 结尾
        const std::size_t len = strnlen(reinterpret_cast<const char*>(data), data_len);
        PortInfo info;
        info.name.assign(reinterpret_cast<const char*>(data), len);
        out.push_back(std::move(info));
    }
#else
    namespace fs = std::filesystem;
    const char* kPrefixes[] = {"ttyUSB", "ttyACM", "ttyS", "ttyAMA", "rfcomm"};
    std::error_code ec;
    for (fs::directory_iterator it("/dev", fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        const std::string file = it->path().filename().string();
        for (const char* p : kPrefixes) {
            if (file.rfind(p, 0) == 0) {
                out.push_back(PortInfo{it->path().string(), {}});
                break;
            }
        }
    }
    // /dev/serial/by-id 下的名字能区分设备型号，比 ttyUSB 编号稳定，附在后面
    std::error_code ec2;
    for (fs::directory_iterator it("/dev/serial/by-id", fs::directory_options::skip_permission_denied,
                                   ec2),
         end;
         !ec2 && it != end; it.increment(ec2)) {
        out.push_back(PortInfo{it->path().string(), it->path().filename().string()});
    }
#endif
    std::sort(out.begin(), out.end(),
              [](const PortInfo& a, const PortInfo& b) { return a.name < b.name; });
    return out;
}

}  // namespace my::serial
