// =============================================================================
//  serial 组件的自测
//
//  分两层，原因是"串口"没法凭空存在：
//    1. 参数校验 / 端口枚举 / 移动语义 —— 任何平台都能测，不需要设备；
//    2. 真正的收发 —— 在 POSIX 上用 **PTY 对**（posix_openpt 造的伪终端）当被试设备：
//       master 端当作"对端设备"，slave 端用 my::serial::Port 打开，于是 read/write/
//       超时/行读取/清缓冲全部能被真实驱动路径覆盖。
//       Windows 上没法凭空造无名 COM 口（需要 com0com 这类驱动），这部分用例 MYTEST_SKIP，
//       并在 CI 的 Linux/qemu 两个 job 里真跑。
// =============================================================================

#include "my_cpp_libs/serial/serial.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "test_framework.hpp"

#if defined(__unix__) || defined(__APPLE__)
#    include <fcntl.h>
#    include <poll.h>
#    include <stdlib.h>
#    include <unistd.h>
#endif

MYTEST_SUITE("serial");

namespace {

namespace ms = my::serial;
using namespace std::chrono_literals;

#if defined(__unix__) || defined(__APPLE__)

/// 一对伪终端：master 当"对端设备"，slave 路径交给 Port::open。
struct PtyPair {
    int         master = -1;
    std::string slave_path;

    PtyPair() = default;
    PtyPair(const PtyPair&)            = delete;
    PtyPair& operator=(const PtyPair&) = delete;
    ~PtyPair() {
        if (master >= 0) ::close(master);
    }
};

bool make_pty(PtyPair& out) {
    const int m = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0) return false;
    if (::grantpt(m) != 0 || ::unlockpt(m) != 0) {
        ::close(m);
        return false;
    }
    const char* name = ::ptsname(m);
    if (name == nullptr) {
        ::close(m);
        return false;
    }
    out.master     = m;
    out.slave_path = name;
    return true;
}

/// 模拟"对端设备发来数据"
bool master_write(int fd, const std::string& s) {
    std::size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = ::write(fd, s.data() + off, s.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

/// 模拟"对端设备收数据"，最多等 timeout
std::string master_read(int fd, std::size_t max_n, std::chrono::milliseconds timeout) {
    std::string out;
    const auto  deadline = std::chrono::steady_clock::now() + timeout;
    while (out.size() < max_n) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) break;
        pollfd p{};
        p.fd     = fd;
        p.events = POLLIN;
        const int rc = ::poll(&p, 1, static_cast<int>(left));
        if (rc <= 0) break;
        char          buf[512];
        const std::size_t want = std::min(sizeof(buf), max_n - out.size());
        const ssize_t     n    = ::read(fd, buf, want);
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
        } else if (n < 0 && errno != EINTR) {
            break;
        }
    }
    return out;
}

#endif  // PTY 可用

}  // namespace

// =============================================================================
//  一、不需要设备的用例（所有平台）
// =============================================================================
MYTEST_CASE(serial_validate_rejects_bad_params) {
    ms::Config c;

    c.baud = 0;
    auto st = ms::validate(c);
    CHECK(!st.ok());
    CHECK_EQ(st.code(), my::Errc::InvalidArgument);

    c.baud      = 115200;
    c.data_bits = 4;
    st          = ms::validate(c);
    CHECK(!st.ok());
    CHECK_EQ(st.code(), my::Errc::InvalidArgument);

    c.data_bits = 9;
    CHECK_EQ(ms::validate(c).code(), my::Errc::InvalidArgument);

    c.data_bits = 8;
    CHECK(ms::validate(c).ok());

    // 5 位数据是合法的（老设备），不该被参数校验挡掉
    c.data_bits = 5;
    CHECK(ms::validate(c).ok());
}

MYTEST_CASE(serial_baud_table_matches_platform) {
#if defined(_WIN32)
    // Windows 的 DCB.BaudRate 接受任意值，没有"标准档位"这回事
    CHECK(ms::standard_bauds().empty());
    CHECK(ms::is_baud_supported(115200));
    CHECK(ms::is_baud_supported(12345));  // 任意值都可能成功，交给 SetCommState 判断
    ms::Config c;
    c.baud = 12345;
    CHECK(ms::validate(c).ok());
#else
    const auto& bauds = ms::standard_bauds();
    CHECK(bauds.size() >= 10);
    // 表必须是升序的（list_ports 风格的一致性，也便于二分查找）
    for (std::size_t i = 1; i < bauds.size(); ++i) {
        CHECK(bauds[i - 1] < bauds[i]);
    }
    CHECK(ms::is_baud_supported(9600));
    CHECK(ms::is_baud_supported(115200));
    CHECK(ms::is_baud_supported(921600));
    CHECK(!ms::is_baud_supported(12345));

    ms::Config c;
    c.baud = 12345;
    auto st = ms::validate(c);
    CHECK(!st.ok());
    CHECK_EQ(st.code(), my::Errc::InvalidArgument);
    // 报错里要给出支持的范围，而不是只说"不支持"
    CHECK(st.message().find("12345") != std::string::npos);
    CHECK(st.message().find("115200") != std::string::npos);
#endif
}

MYTEST_CASE(serial_open_missing_device_reports_clearly) {
    auto r = ms::Port::open("/dev/definitely-not-a-serial-port-xyz");
    CHECK(!r.ok());
    // 消息里要带上被打开的路径，否则板端 `ls /dev` 都排查不了
    CHECK(r.status().message().find("definitely-not-a-serial-port-xyz") != std::string::npos);
#if !defined(_WIN32)
    CHECK_EQ(r.status().code(), my::Errc::NotFound);
#endif
}

MYTEST_CASE(serial_open_with_empty_name) {
    auto r = ms::Port::open("");
    CHECK(!r.ok());
    CHECK_EQ(r.status().code(), my::Errc::InvalidArgument);
}

MYTEST_CASE(serial_validate_runs_before_touching_device) {
    // 参数非法 + 设备不存在时，应该先报参数问题（说明是调用方写错了）
    ms::Config c;
    c.data_bits = 3;
    auto r      = ms::Port::open("/dev/definitely-not-a-serial-port-xyz", c);
    CHECK(!r.ok());
    CHECK_EQ(r.status().code(), my::Errc::InvalidArgument);
}

MYTEST_CASE(serial_list_ports_is_sane) {
    auto r = ms::list_ports();
    CHECK(r.ok());
    const auto& ports = r.value();
    for (const auto& p : ports) {
        EXPECT(!p.name.empty());
    }
    // 排序（名字升序），便于 UI 稳定显示
    for (std::size_t i = 1; i < ports.size(); ++i) {
        CHECK(ports[i - 1].name <= ports[i].name);
    }
}

MYTEST_CASE(serial_default_port_is_closed) {
    ms::Port p;
    CHECK(!p.is_open());
    CHECK(p.name().empty());
    // 未打开时的操作必须返回明确错误，而不是越界/崩溃
    std::uint8_t buf[4] = {};
    auto         rd     = p.read_some(buf, sizeof buf, 10ms);
    CHECK(!rd.ok());
    CHECK_EQ(rd.status().code(), my::Errc::NotInitialized);
    auto wr = p.write_some(buf, sizeof buf, 10ms);
    CHECK(!wr.ok());
    CHECK_EQ(wr.status().code(), my::Errc::NotInitialized);
    auto avail = p.bytes_available();
    CHECK(!avail.ok());
    // 关闭一个没打开的端口是幂等的
    CHECK(p.close().ok());
    CHECK(p.close().ok());
}

MYTEST_CASE(serial_move_semantics_keeps_ownership) {
    // 先覆盖"空对象移动"这条平凡路径
    ms::Port empty;
    ms::Port moved(std::move(empty));
    CHECK(!empty.is_open());
    CHECK(!moved.is_open());

#if defined(__unix__) || defined(__APPLE__)
    // 关键：移动一个**已经打开**的 Port，句柄必须跟着走，而且还能收发。
    // 以前这条用例全程只用默认构造的 closed Port，移动构造完全不搬 handle_ 也会绿。
    PtyPair pty;
    if (!make_pty(pty)) MYTEST_SKIP("本机无法创建 PTY");

    CHECK_OK_VALUE(src, ms::Port::open(pty.slave_path));
    const std::intptr_t raw = src.native_handle();
    CHECK(raw >= 0);

    ms::Port dst(std::move(src));
    CHECK(!src.is_open());                 // 源对象必须放弃句柄
    CHECK(dst.is_open());
    CHECK_EQ(dst.native_handle(), raw);    // 句柄真的搬过去了
    CHECK_EQ(dst.name(), pty.slave_path);

    CHECK(master_write(pty.master, "MOVE"));
    std::uint8_t buf[8] = {};
    auto         got    = dst.read_some(buf, sizeof buf, 1000ms);
    CHECK_OK_VALUE(n, got);
    CHECK_EQ(std::string(reinterpret_cast<char*>(buf), n), std::string("MOVE"));

    // 移动赋值同样要把句柄接过来
    ms::Port assignee;
    assignee = std::move(dst);
    CHECK(!dst.is_open());
    CHECK(assignee.is_open());
    CHECK(master_write(pty.master, "OK"));
    auto got2 = assignee.read_some(buf, sizeof buf, 1000ms);
    CHECK_OK_VALUE(n2, got2);
    CHECK_EQ(std::string(reinterpret_cast<char*>(buf), n2), std::string("OK"));
    CHECK(assignee.close().ok());
#else
    MYTEST_SKIP("Windows 上需要虚拟串口驱动才能验证句柄转移");
#endif
}

// =============================================================================
//  二、PTY 上的真实收发（仅 POSIX）
// =============================================================================
#if defined(__unix__) || defined(__APPLE__)

MYTEST_CASE(serial_pty_roundtrip) {
    PtyPair pty;
    if (!make_pty(pty)) MYTEST_SKIP("本机无法创建 PTY（posix_openpt 失败）");

    ms::Config cfg;
    cfg.baud = 115200;
    CHECK_OK_VALUE(port, ms::Port::open(pty.slave_path, cfg));
    CHECK(port.is_open());
    CHECK_EQ(port.name(), pty.slave_path);

    // 对端 -> 本库
    CHECK(master_write(pty.master, "HELLO"));
    std::uint8_t buf[32] = {};
    auto         got     = port.read_some(buf, sizeof buf, 1000ms);
    CHECK_OK_VALUE(n, got);
    CHECK_EQ(n, std::size_t{5});
    CHECK_EQ(std::string(reinterpret_cast<char*>(buf), n), std::string("HELLO"));

    // 本库 -> 对端
    const char out[] = "WORLD!";
    CHECK(port.write_all(out, sizeof(out) - 1, 1000ms).ok());
    CHECK_EQ(master_read(pty.master, 64, 1000ms), std::string("WORLD!"));

    CHECK(port.close().ok());
    CHECK(!port.is_open());
}

// write_some 的超时契约：对端不读时不能无限阻塞；0ms 是"尽力而为"，
// 有空间可以写出部分，没空间就返回 0（Ok），不是 IoError。
MYTEST_CASE(serial_pty_write_some_timeout_is_bounded) {
    PtyPair pty;
    if (!make_pty(pty)) MYTEST_SKIP("本机无法创建 PTY");

    CHECK_OK_VALUE(port, ms::Port::open(pty.slave_path));

    std::vector<std::uint8_t> big(256 * 1024, 0x5A);  // 远大于 PTY 内核缓冲
    const auto                t0 = std::chrono::steady_clock::now();
    auto                      wr = port.write_some(big.data(), big.size(), 50ms);
    const auto                elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();

    CHECK_OK_VALUE(written, wr);
    CHECK(elapsed < 1000);          // 不能因为对端不读而挂死
    CHECK(written <= big.size());

    // 再确认 0ms 也不会阻塞（允许写出 0 或一部分）
    const auto t1 = std::chrono::steady_clock::now();
    auto       wr0 = port.write_some(big.data(), big.size(), 0ms);
    const auto elapsed0 = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - t1)
                              .count();
    CHECK_OK_VALUE(written0, wr0);
    CHECK(elapsed0 < 1000);
    CHECK(written0 <= big.size());
}

MYTEST_CASE(serial_pty_read_timeout_returns_zero) {
    PtyPair pty;
    if (!make_pty(pty)) MYTEST_SKIP("本机无法创建 PTY");

    CHECK_OK_VALUE(port, ms::Port::open(pty.slave_path));

    std::uint8_t buf[8] = {};
    const auto   t0     = std::chrono::steady_clock::now();
    auto         got    = port.read_some(buf, sizeof buf, 80ms);
    const auto   elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();

    // 关键契约：超时 = 0 字节 + Ok，不是错误（串口轮询是常态）
    CHECK(got.ok());
    CHECK_EQ(got.value(), std::size_t{0});
    // 真的等了（不是立刻返回），但也不该远超 80ms
    CHECK(elapsed >= 60);
    CHECK(elapsed < 2000);
}

MYTEST_CASE(serial_pty_line_read) {
    PtyPair pty;
    if (!make_pty(pty)) MYTEST_SKIP("本机无法创建 PTY");

    CHECK_OK_VALUE(port, ms::Port::open(pty.slave_path));

    CHECK(master_write(pty.master, "PING\nPONG\n"));

    auto l1 = port.read_line('\n', 64, 1000ms);
    CHECK_OK_VALUE(s1, l1);
    CHECK_EQ(s1, std::string("PING"));  // 不含行尾

    auto l2 = port.read_line('\n', 64, 1000ms);
    CHECK_OK_VALUE(s2, l2);
    CHECK_EQ(s2, std::string("PONG"));
}

MYTEST_CASE(serial_pty_line_read_respects_max_len) {
    PtyPair pty;
    if (!make_pty(pty)) MYTEST_SKIP("本机无法创建 PTY");

    CHECK_OK_VALUE(port, ms::Port::open(pty.slave_path));

    // 100 个字节没有换行：读满 max_len 就该返回，不能无限等
    CHECK(master_write(pty.master, std::string(100, 'x')));
    auto line = port.read_line('\n', 10, 500ms);
    CHECK_OK_VALUE(s, line);
    CHECK_EQ(s.size(), std::size_t{10});
}

MYTEST_CASE(serial_pty_line_read_timeout_returns_partial) {
    PtyPair pty;
    if (!make_pty(pty)) MYTEST_SKIP("本机无法创建 PTY");

    CHECK_OK_VALUE(port, ms::Port::open(pty.slave_path));

    // 只有 'A'，没有换行：超时后应返回已经读到的部分
    CHECK(master_write(pty.master, "A"));
    auto line = port.read_line('\n', 64, 100ms);
    CHECK_OK_VALUE(s, line);
    CHECK_EQ(s, std::string("A"));
}

MYTEST_CASE(serial_pty_bytes_available_and_flush) {
    PtyPair pty;
    if (!make_pty(pty)) MYTEST_SKIP("本机无法创建 PTY");

    CHECK_OK_VALUE(port, ms::Port::open(pty.slave_path));

    CHECK(master_write(pty.master, "12345"));

    // 数据从 master 到 slave 是异步的，给它一点时间
    std::size_t avail = 0;
    for (int i = 0; i < 50; ++i) {
        auto a = port.bytes_available();
        CHECK_OK_VALUE(v, a);
        avail = v;
        if (avail >= 5) break;
        std::this_thread::sleep_for(2ms);
    }
    CHECK_EQ(avail, std::size_t{5});

    CHECK(port.flush().ok());
    auto after = port.bytes_available();
    CHECK_OK_VALUE(v2, after);
    CHECK_EQ(v2, std::size_t{0});
}

MYTEST_CASE(serial_pty_close_then_io_fails) {
    PtyPair pty;
    if (!make_pty(pty)) MYTEST_SKIP("本机无法创建 PTY");

    CHECK_OK_VALUE(port, ms::Port::open(pty.slave_path));
    CHECK(port.close().ok());

    std::uint8_t buf[4] = {};
    auto         rd     = port.read_some(buf, sizeof buf, 10ms);
    CHECK(!rd.ok());
    CHECK_EQ(rd.status().code(), my::Errc::NotInitialized);

    auto wr = port.write_some(buf, 1, 10ms);
    CHECK(!wr.ok());
    CHECK_EQ(wr.status().code(), my::Errc::NotInitialized);
}

// 回归：PTY master 关闭（对端"拔线"）后，slave 的 poll 会立刻返回 POLLHUP，
// 随后 read() 返回 0。修复前只等 POLLIN、不看 revents，read_some(500ms) 在 µs 级
// 返回 Ok/0，文档推荐的 `if (n == 0) continue;` 会 100% CPU 空转。
// 现在必须立刻报 Errc::IoError，而不是等满 500ms 超时或返回 0 字节。
MYTEST_CASE(serial_pty_peer_close_reports_io_error) {
    PtyPair pty;
    if (!make_pty(pty)) MYTEST_SKIP("本机无法创建 PTY");

    CHECK_OK_VALUE(port, ms::Port::open(pty.slave_path));

    // 关闭 master，相当于对端设备断开
    const int close_rc = ::close(pty.master);
    pty.master         = -1;  // 避免析构时重复 close（fd 可能已被别的线程复用）
    CHECK_EQ(close_rc, 0);

    std::uint8_t buf[16] = {};
    const auto   t0      = std::chrono::steady_clock::now();
    auto         rd      = port.read_some(buf, sizeof buf, 500ms);
    const auto   elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - t0)
                              .count();

    CHECK(!rd.ok());
    CHECK_EQ(rd.status().code(), my::Errc::IoError);
    CHECK(elapsed < 200);  // 挂断必须立刻返回，不能等满 500ms
    CHECK(rd.status().message().find("挂断") != std::string::npos ||
          rd.status().message().find("EOF") != std::string::npos);

    // read_line 同样要把挂断透传成错误，不能返回空串 + Ok 让上层继续轮询
    auto line = port.read_line('\n', 64, 500ms);
    CHECK(!line.ok());
}

MYTEST_CASE(serial_pty_write_all_times_out_when_peer_never_reads) {
    PtyPair pty;
    if (!make_pty(pty)) MYTEST_SKIP("本机无法创建 PTY");

    CHECK_OK_VALUE(port, ms::Port::open(pty.slave_path));

    // 64 KB 远超 PTY 的缓冲；对端一个字节都不读 -> 必须超时返回，而不是永久阻塞
    const std::string big(64 * 1024, 'z');
    const auto        t0 = std::chrono::steady_clock::now();
    auto              st = port.write_all(big.data(), big.size(), 100ms);
    const auto        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();

    CHECK(!st.ok());
    CHECK_EQ(st.code(), my::Errc::Timeout);
    // 报错里要能看出写了多少，否则调用方无法决定重发哪一段
    CHECK(st.message().find("已写") != std::string::npos);
    CHECK(elapsed < 5000);
}

MYTEST_CASE(serial_pty_two_ports_are_independent) {
    // 两个同时打开的 PTY 互不干扰（验证句柄状态没有做成全局）
    PtyPair a, b;
    if (!make_pty(a) || !make_pty(b)) MYTEST_SKIP("本机无法创建 PTY");

    CHECK_OK_VALUE(pa, ms::Port::open(a.slave_path));
    CHECK_OK_VALUE(pb, ms::Port::open(b.slave_path));

    CHECK(master_write(a.master, "AAA"));
    CHECK(master_write(b.master, "BBB"));

    std::uint8_t bufa[8] = {}, bufb[8] = {};
    auto         ra      = pa.read_some(bufa, sizeof bufa, 1000ms);
    auto         rb      = pb.read_some(bufb, sizeof bufb, 1000ms);
    CHECK_OK_VALUE(na, ra);
    CHECK_OK_VALUE(nb, rb);
    CHECK_EQ(std::string(reinterpret_cast<char*>(bufa), na), std::string("AAA"));
    CHECK_EQ(std::string(reinterpret_cast<char*>(bufb), nb), std::string("BBB"));
}

#else  //  _WIN32

// Windows 上没法凭空造 unnamed COM 口（要装 com0com 之类的驱动），
// 所以真实收发这部分在 CI 的 Linux / QEMU job 上验证。
MYTEST_CASE(serial_pty_roundtrip) { MYTEST_SKIP("Windows 上需要虚拟串口驱动才能测收发"); }
MYTEST_CASE(serial_pty_read_timeout_returns_zero) {
    MYTEST_SKIP("Windows 上需要虚拟串口驱动");
}
MYTEST_CASE(serial_pty_line_read) { MYTEST_SKIP("Windows 上需要虚拟串口驱动"); }
MYTEST_CASE(serial_pty_line_read_respects_max_len) {
    MYTEST_SKIP("Windows 上需要虚拟串口驱动");
}
MYTEST_CASE(serial_pty_line_read_timeout_returns_partial) {
    MYTEST_SKIP("Windows 上需要虚拟串口驱动");
}
MYTEST_CASE(serial_pty_bytes_available_and_flush) {
    MYTEST_SKIP("Windows 上需要虚拟串口驱动");
}
MYTEST_CASE(serial_pty_close_then_io_fails) { MYTEST_SKIP("Windows 上需要虚拟串口驱动"); }
MYTEST_CASE(serial_pty_write_all_times_out_when_peer_never_reads) {
    MYTEST_SKIP("Windows 上需要虚拟串口驱动");
}
MYTEST_CASE(serial_pty_write_some_timeout_is_bounded) {
    MYTEST_SKIP("Windows 上需要虚拟串口驱动");
}
MYTEST_CASE(serial_pty_two_ports_are_independent) {
    MYTEST_SKIP("Windows 上需要虚拟串口驱动");
}
MYTEST_CASE(serial_pty_peer_close_reports_io_error) {
    MYTEST_SKIP("Windows 上需要虚拟串口驱动");
}

#endif
