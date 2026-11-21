// =============================================================================
//  my::net 单元测试（TCP/UDP 回环，不需要外部网络）
//
//  重点覆盖三件事：
//    1. 平台差异有没有吃干净（超时、对端关闭、SIGPIPE 不杀进程）；
//    2. 分帧/行协议这两种板端最常用的应用层格式；
//    3. 错误路径给的是不是 Status（而不是 errno 或者崩溃）。
// =============================================================================

#include "my_cpp_libs/net/net.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "test_framework.hpp"

MYTEST_SUITE("net");

namespace {

using namespace std::chrono_literals;

/// 断言失败时宏里是 `return`，裸 std::thread 会在析构时 terminate —— 用 RAII 兜住
struct JoinGuard {
    std::thread t;
    explicit JoinGuard(std::thread&& th) : t(std::move(th)) {}
    ~JoinGuard() {
        if (t.joinable()) t.join();
    }
    JoinGuard(const JoinGuard&)            = delete;
    JoinGuard& operator=(const JoinGuard&) = delete;
};

/// 在后台线程跑一个 echo 服务：接受 1 个连接，读什么回什么，直到对端关闭
struct EchoServer {
    my::net::TcpListener        listener;
    std::thread                 thread;
    std::atomic<int>            conns{0};
    std::atomic<bool>           finished{false};

    explicit EchoServer(my::net::TcpListener&& l) : listener(std::move(l)) {
        thread = std::thread([this] {
            auto c = listener.accept(5000);
            if (!c) {
                finished = true;
                return;
            }
            conns = 1;
            // 给连接设超时：客户端异常时线程不会永久阻塞在 recv 上（否则析构 join 会挂死）
            CHECK_OK(c.value().set_timeout(3000));
            std::uint8_t buf[4096];
            while (true) {
                auto r = c.value().recv(buf, sizeof(buf));
                if (!r || r.value() == 0) break;
                if (!c.value().send_all(buf, r.value())) break;
            }
            finished = true;
        });
    }
    ~EchoServer() {
        listener.close();
        if (thread.joinable()) thread.join();
    }
    std::uint16_t port() const { return listener.port(); }
};

}  // namespace

// -----------------------------------------------------------------------------
//  地址解析
// -----------------------------------------------------------------------------
MYTEST_CASE(net_parse_endpoint_forms) {
    CHECK_OK_VALUE(a, my::net::parse_endpoint("127.0.0.1:9000"));
    CHECK(a.host == "127.0.0.1");
    CHECK_EQ(a.port, std::uint16_t{9000});

    CHECK_OK_VALUE(b, my::net::parse_endpoint("localhost", 8080));
    CHECK(b.host == "localhost");
    CHECK_EQ(b.port, std::uint16_t{8080});

    CHECK_OK_VALUE(c, my::net::parse_endpoint(":5000"));
    CHECK(c.host.empty());
    CHECK_EQ(c.port, std::uint16_t{5000});

    CHECK_OK_VALUE(d, my::net::parse_endpoint("[::1]:7000"));
    CHECK(d.host == "::1");
    CHECK_EQ(d.port, std::uint16_t{7000});

    CHECK_ERR(my::net::parse_endpoint(""));            // 空
    CHECK_ERR(my::net::parse_endpoint("host:99999"));  // 端口越界
    CHECK_ERR(my::net::parse_endpoint("host:abc"));    // 端口非数字
}

// -----------------------------------------------------------------------------
//  TCP
// -----------------------------------------------------------------------------
MYTEST_CASE(net_tcp_echo_roundtrip) {
    auto l = my::net::TcpListener::bind({"127.0.0.1", 0}, 4);
    CHECK_RESULT(l);
    EchoServer server(std::move(l.value()));
    CHECK(server.port() != 0);

    auto cli = my::net::TcpSocket::connect({"127.0.0.1", server.port()}, 3000);
    CHECK_RESULT(cli);
    CHECK_EQ(cli.value().peer().port, server.port());   // getpeername 能取到对端
    CHECK(!cli.value().local().host.empty());           // getsockname 能取到本地地址

    const std::string msg = "hello my::net";
    CHECK_OK(cli.value().send_all(msg.data(), msg.size()));

    auto got = cli.value().recv_exact(msg.size());
    CHECK_RESULT(got);
    CHECK_EQ(std::string(got.value().begin(), got.value().end()), msg);

    CHECK_EQ(server.conns.load(), 1);   // 服务端确实接受到了连接

    cli.value().close();
    // 客户端关闭后，服务端线程应当自己退出（有超时兜底，不会拖死）
    for (int i = 0; i < 60 && !server.finished.load(); ++i) std::this_thread::sleep_for(50ms);
    CHECK(server.finished.load());
}

MYTEST_CASE(net_tcp_accept_timeout) {
    auto l = my::net::TcpListener::bind({"127.0.0.1", 0}, 4);
    CHECK_RESULT(l);

    const auto t0 = std::chrono::steady_clock::now();
    auto       c  = l.value().accept(120);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();

    CHECK(!c);
    CHECK_EQ(c.status().code(), my::Errc::Timeout);
    CHECK(ms >= 100);  // 真的等了一会儿，不是立即返回
}

MYTEST_CASE(net_tcp_connect_refused) {
    // 先占一个端口再释放，然后去连它 —— 正常没人监听
    std::uint16_t port = 0;
    {
        auto l = my::net::TcpListener::bind({"127.0.0.1", 0}, 1);
        CHECK_RESULT(l);
        port = l.value().port();
        l.value().close();
    }
    auto c = my::net::TcpSocket::connect({"127.0.0.1", port}, 500);
    CHECK(!c);
    // 拒绝连接 / 超时 都算合理（不同平台错误码不同）
    CHECK(c.status().code() == my::Errc::IoError || c.status().code() == my::Errc::Timeout);
}

MYTEST_CASE(net_tcp_peer_close_is_zero) {
    auto l = my::net::TcpListener::bind({"127.0.0.1", 0}, 1);
    CHECK_RESULT(l);

    JoinGuard peer(std::thread([&] {
        auto c = l.value().accept(3000);
        if (c) c.value().close();  // 立刻关闭
    }));

    auto cli = my::net::TcpSocket::connect({"127.0.0.1", l.value().port()}, 3000);
    CHECK_RESULT(cli);
    CHECK_OK(cli.value().set_timeout(1000));

    // 对端关闭后，recv 返回 0（而不是错误）
    std::uint8_t buf[16];
    auto         r = cli.value().recv(buf, sizeof(buf));
    CHECK_RESULT(r);
    CHECK_EQ(r.value(), std::size_t{0});

}

MYTEST_CASE(net_tcp_send_after_peer_close_does_not_crash) {
    // Linux 上往已关闭的连接写会收到 SIGPIPE（默认杀进程）。
    // 我们内部用 MSG_NOSIGNAL，必须表现为 Status 而不是进程退出。
    auto l = my::net::TcpListener::bind({"127.0.0.1", 0}, 1);
    CHECK_RESULT(l);

    JoinGuard peer(std::thread([&] {
        auto c = l.value().accept(3000);
        if (c) c.value().close();
    }));

    auto cli = my::net::TcpSocket::connect({"127.0.0.1", l.value().port()}, 3000);
    CHECK_RESULT(cli);
    CHECK_OK(cli.value().set_timeout(300));

    if (peer.t.joinable()) peer.t.join();   // 先确保对端真的关了，再往死连接写
    std::this_thread::sleep_for(100ms);

    std::vector<std::uint8_t> big(256 * 1024, 0x5A);
    bool     got_error = false;
    my::Errc code      = my::Errc::Ok;
    // 进程还活着本身就是断言的一部分：如果实现漏了 MSG_NOSIGNAL，第一条 send
    // 就会因为 SIGPIPE 杀掉进程，走不到下面任何一行。休眠后 RST 已经到达，
    // 往死连接写必须表现为 Status —— 这里给足次数，不能靠"碰巧第一次就失败"。
    for (int i = 0; i < 32 && !got_error; ++i) {
        auto st = cli.value().send_all(big.data(), big.size());
        if (!st) {
            got_error = true;
            code      = st.code();
        }
    }
    // 原来的判据 `got_error || valid()` 是恒真的（valid() 就是 fd_ >= 0，
    // 而发送失败并不会关闭 socket），等于什么都没验。真正要验的是：
    //   1) 写死连接报的是 I/O 类错误，而不是静默成功；
    //   2) 失败后句柄仍然有效（调用方可能还要 shutdown_write 收尾）。
    CHECK(got_error);
    CHECK(code == my::Errc::IoError || code == my::Errc::RuntimeError);
    CHECK(cli.value().valid());
}

MYTEST_CASE(net_tcp_closed_socket_reports_not_initialized) {
    my::net::TcpSocket s;
    CHECK(!s.valid());
    auto r = s.send("x", 1);
    CHECK(!r);
    CHECK_EQ(r.status().code(), my::Errc::NotInitialized);
    CHECK_ERR(s.set_timeout(10));
}

// -----------------------------------------------------------------------------
//  分帧
// -----------------------------------------------------------------------------
namespace {

/// 建一对回环 socket：server 侧、client 侧
struct SocketPair {
    my::net::TcpSocket a;  // client 端
    my::net::TcpSocket b;  // server 端
    my::net::TcpListener listener;

    static my::Result<SocketPair> make() {
        auto l = my::net::TcpListener::bind({"127.0.0.1", 0}, 1);
        if (!l) return l.status();

        SocketPair p;
        p.listener = std::move(l.value());

        auto client = my::net::TcpSocket::connect({"127.0.0.1", p.listener.port()}, 3000);
        if (!client) return client.status();
        auto server = p.listener.accept(3000);
        if (!server) return server.status();

        p.a = std::move(client.value());
        p.b = std::move(server.value());
        (void)p.a.set_timeout(2000);  // make() 返回 Result，这里不能提前 return
        (void)p.b.set_timeout(2000);
        return p;
    }
    SocketPair() = default;
    SocketPair(SocketPair&&) = default;
    SocketPair& operator=(SocketPair&&) = default;
    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;
    ~SocketPair() = default;
};

}  // namespace

MYTEST_CASE(net_frame_roundtrip) {
    auto p = SocketPair::make();
    CHECK_RESULT(p);

    const std::string payload = "0123456789abcdef";
    CHECK_OK(my::net::write_frame(p.value().a, payload));

    auto got = my::net::read_frame(p.value().b, 1024);
    CHECK_RESULT(got);
    CHECK_EQ(std::string(got.value().begin(), got.value().end()), payload);

    // 空帧也要能过
    CHECK_OK(my::net::write_frame(p.value().a, std::string{}));
    auto empty = my::net::read_frame(p.value().b, 1024);
    CHECK_RESULT(empty);
    CHECK(empty.value().empty());
}

MYTEST_CASE(net_frame_rejects_oversized_length) {
    auto p = SocketPair::make();
    CHECK_RESULT(p);

    // 手工发一个长度字段 = 0xFFFFFFFF 的帧头，读端应当拒绝而不是去分配 4GB
    const std::uint8_t head[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    CHECK_OK(p.value().a.send_all(head, 4));

    auto got = my::net::read_frame(p.value().b, 1024);
    CHECK(!got);
    CHECK_EQ(got.status().code(), my::Errc::OutOfRange);
    // 长度头已消费、负载无法安全丢弃：标记错位，不能重试后错位
    CHECK(p.value().b.valid());
    CHECK(p.value().b.desynced());
    auto retry = my::net::read_frame(p.value().b, 1024);
    CHECK(!retry);
    CHECK_EQ(retry.status().code(), my::Errc::RuntimeError);
}

MYTEST_CASE(net_read_line) {
    auto p = SocketPair::make();
    CHECK_RESULT(p);

    // 最后一行没有换行符：靠半关闭写端触发 EOF，read_line 应当把它当作完整一行返回
    const std::string text = "hello\r\nworld\nlast";
    CHECK_OK(p.value().a.send_all(text.data(), text.size()));
    CHECK_OK(p.value().a.shutdown_write());

    std::string line;
    CHECK_OK(my::net::read_line(p.value().b, line));
    CHECK(line == "hello");

    CHECK_OK(my::net::read_line(p.value().b, line));
    CHECK(line == "world");

    CHECK_OK(my::net::read_line(p.value().b, line));
    CHECK(line == "last");
}

MYTEST_CASE(net_read_line_rejects_overlong) {
    auto p = SocketPair::make();
    CHECK_RESULT(p);

    std::string junk(1000, 'x');
    CHECK_OK(p.value().a.send_all(junk.data(), junk.size()));

    std::string line;
    auto        st = my::net::read_line(p.value().b, line, /*max_len=*/64);
    CHECK(!st);
    CHECK_EQ(st.code(), my::Errc::OutOfRange);
    // 半行已经消费：标记错位，后续分帧调用必须拒绝
    CHECK(p.value().b.desynced());
    std::string again;
    auto        retry = my::net::read_line(p.value().b, again, 64);
    CHECK(!retry);
    CHECK_EQ(retry.code(), my::Errc::RuntimeError);
}

MYTEST_CASE(net_recv_timeout) {
    auto p = SocketPair::make();
    CHECK_RESULT(p);
    CHECK_OK(p.value().b.set_timeout(100));

    std::uint8_t byte = 0;
    const auto   t0   = std::chrono::steady_clock::now();
    auto         r    = p.value().b.recv(&byte, 1);
    const auto   ms   = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    CHECK(!r);
    CHECK_EQ(r.status().code(), my::Errc::Timeout);
    CHECK(ms >= 80);
    // 一个字节都没收到：连接仍可重试，不能被误标记为错位
    CHECK(p.value().b.valid());
    CHECK(!p.value().b.desynced());
}

// set_timeout(0) 恢复阻塞；先收一次超时，再验证后续阻塞 recv 能拿到数据。
MYTEST_CASE(net_set_timeout_zero_restores_blocking) {
    auto p = SocketPair::make();
    CHECK_RESULT(p);

    CHECK_OK(p.value().b.set_timeout(80));
    std::uint8_t byte = 0;
    auto         first = p.value().b.recv(&byte, 1);
    CHECK(!first);
    CHECK_EQ(first.status().code(), my::Errc::Timeout);

    CHECK_OK(p.value().b.set_timeout(0));
    std::thread sender([&] {
        std::this_thread::sleep_for(50ms);
        (void)p.value().a.send_all("z", 1);
    });

    auto got = p.value().b.recv(&byte, 1);
    if (sender.joinable()) sender.join();
    CHECK_RESULT(got);
    CHECK_EQ(got.value(), std::size_t{1});
    CHECK_EQ(byte, static_cast<std::uint8_t>('z'));
}

MYTEST_CASE(net_read_frame_split_header) {
    auto p = SocketPair::make();
    CHECK_RESULT(p);

    const std::string payload = "split-frame";
    const std::uint32_t n     = static_cast<std::uint32_t>(payload.size());
    const std::uint8_t  head[4] = {
        static_cast<std::uint8_t>((n >> 24) & 0xFF), static_cast<std::uint8_t>((n >> 16) & 0xFF),
        static_cast<std::uint8_t>((n >> 8) & 0xFF), static_cast<std::uint8_t>(n & 0xFF)};

    // 长度头 1 字节 1 字节地发，中间留出调度间隙；接收端必须自己攒满 4 字节
    for (std::size_t i = 0; i < sizeof(head); ++i) {
        CHECK_OK(p.value().a.send_all(&head[i], 1));
        std::this_thread::sleep_for(10ms);
    }
    CHECK_OK(p.value().a.send_all(payload.data(), payload.size()));

    auto got = my::net::read_frame(p.value().b, 1024);
    CHECK_RESULT(got);
    CHECK_EQ(std::string(got.value().begin(), got.value().end()), payload);
}

MYTEST_CASE(net_read_frame_partial_payload_timeout_marks_desynced) {
    auto p = SocketPair::make();
    CHECK_RESULT(p);
    CHECK_OK(p.value().b.set_timeout(100));

    const std::uint8_t head[4] = {0, 0, 0, 100};  // 声明 100 字节
    const std::uint8_t part[3] = {1, 2, 3};
    CHECK_OK(p.value().a.send_all(head, sizeof(head)));
    CHECK_OK(p.value().a.send_all(part, sizeof(part)));

    auto got = my::net::read_frame(p.value().b, 1024);
    CHECK(!got);
    CHECK_EQ(got.status().code(), my::Errc::Timeout);
    CHECK(p.value().b.valid());
    CHECK(p.value().b.desynced());
}

MYTEST_CASE(net_recv_exact_partial_timeout_marks_desynced) {    auto p = SocketPair::make();
    CHECK_RESULT(p);

    const std::uint8_t partial[3] = {1, 2, 3};
    CHECK_OK(p.value().a.send_all(partial, sizeof(partial)));
    CHECK_OK(p.value().b.set_timeout(50));

    auto got = p.value().b.recv_exact(10);
    CHECK(!got);
    CHECK_EQ(got.status().code(), my::Errc::Timeout);
    // 已收 3/10 后超时：句柄保留但流已错位，重试必须明确拒绝而不是读错位数据
    CHECK(p.value().b.valid());
    CHECK(p.value().b.desynced());
    auto retry = p.value().b.recv_exact(10);
    CHECK(!retry);
    CHECK_EQ(retry.status().code(), my::Errc::RuntimeError);
}

// -----------------------------------------------------------------------------
//  UDP
// -----------------------------------------------------------------------------
MYTEST_CASE(net_udp_roundtrip) {
    auto a = my::net::UdpSocket::bind({"127.0.0.1", 0});
    CHECK_RESULT(a);
    auto b = my::net::UdpSocket::bind({"127.0.0.1", 0});
    CHECK_RESULT(b);
    CHECK(a.value().port() != 0);
    CHECK(b.value().port() != 0);

    CHECK_OK(a.value().set_timeout(2000));
    CHECK_OK(b.value().set_timeout(2000));

    const std::string msg = "udp-hello";
    CHECK_RESULT(a.value().send_to({"127.0.0.1", b.value().port()}, msg.data(), msg.size()));

    auto d = b.value().recv_from();
    CHECK_RESULT(d);
    CHECK_EQ(std::string(d.value().data.begin(), d.value().data.end()), msg);
    CHECK_EQ(d.value().from.port, a.value().port());
}

MYTEST_CASE(net_udp_recv_timeout) {
    auto s = my::net::UdpSocket::bind({"127.0.0.1", 0});
    CHECK_RESULT(s);
    CHECK_OK(s.value().set_timeout(120));

    const auto t0 = std::chrono::steady_clock::now();
    auto       d  = s.value().recv_from();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    CHECK(!d);
    CHECK_EQ(d.status().code(), my::Errc::Timeout);
    CHECK(ms >= 100);
}

MYTEST_CASE(net_udp_recv_from_rejects_huge_max_size) {
    auto s = my::net::UdpSocket::bind({"127.0.0.1", 0});
    CHECK_RESULT(s);

    // UDP 数据报最多 65535 字节；传 64 KiB 也应当直接拒绝，不能先分配
    auto d = s.value().recv_from(65536);
    CHECK(!d);
    CHECK_EQ(d.status().code(), my::Errc::OutOfRange);
}

MYTEST_CASE(net_udp_broadcast_flag) {
    auto s = my::net::UdpSocket::open();
    CHECK_RESULT(s);
    CHECK_OK(s.value().set_broadcast(true));
    CHECK_OK(s.value().set_broadcast(false));
}

// 广播数据面：很多 CI/容器默认禁广播，环境不支持时明确 SKIP，不伪装通过。
MYTEST_CASE(net_udp_broadcast_roundtrip) {
    auto rx = my::net::UdpSocket::bind({"0.0.0.0", 0});
    CHECK_RESULT(rx);
    CHECK_OK(rx.value().set_timeout(500));

    auto tx = my::net::UdpSocket::open();
    CHECK_RESULT(tx);
    CHECK_OK(tx.value().set_broadcast(true));

    const std::string msg = "broadcast-data";
    auto sent = tx.value().send_to({"255.255.255.255", rx.value().port()}, msg.data(), msg.size());
    if (!sent) MYTEST_SKIP(("当前环境不允许 UDP 广播: " + sent.status().to_string()).c_str());

    auto d = rx.value().recv_from();
    if (!d) MYTEST_SKIP(("当前环境收不到 UDP 广播: " + d.status().to_string()).c_str());
    CHECK_EQ(std::string(d.value().data.begin(), d.value().data.end()), msg);
}

// 组播数据面：join 失败（无组播路由）时 SKIP；能 join 就必须能收到自己发的包。
MYTEST_CASE(net_udp_multicast_roundtrip) {
    auto rx = my::net::UdpSocket::bind({"0.0.0.0", 0});
    CHECK_RESULT(rx);
    CHECK_OK(rx.value().set_timeout(500));

    const std::string group = "239.255.42.99";
    auto joined = rx.value().join_multicast(group);
    if (!joined) MYTEST_SKIP(("当前环境不支持组播: " + joined.to_string()).c_str());

    auto tx = my::net::UdpSocket::open();
    CHECK_RESULT(tx);
    const std::string msg = "multicast-data";
    auto sent = tx.value().send_to({group, rx.value().port()}, msg.data(), msg.size());
    if (!sent) MYTEST_SKIP(("当前环境不允许发组播: " + sent.status().to_string()).c_str());

    auto d = rx.value().recv_from();
    if (!d) MYTEST_SKIP(("当前环境收不到组播: " + d.status().to_string()).c_str());
    CHECK_EQ(std::string(d.value().data.begin(), d.value().data.end()), msg);
}

MYTEST_CASE(net_udp_multicast_join_rejects_bad_group) {
    auto s = my::net::UdpSocket::bind({"0.0.0.0", 0});
    CHECK_RESULT(s);
    // 非 IPv4 组播地址应当明确报错，而不是静默失败
    auto st = s.value().join_multicast("not-an-ip");
    CHECK(!st);
    CHECK_EQ(st.code(), my::Errc::InvalidArgument);
}

// -----------------------------------------------------------------------------
//  补充覆盖：缓冲区上限、半包、端口占用、大缓冲发送、移动语义、地址解析错误
// -----------------------------------------------------------------------------

MYTEST_CASE(net_recv_exact_rejects_oversized) {
    // 上限检查必须在触碰句柄之前：这里故意用一个**未连接**的 socket，
    // 如果实现先判 valid()，拿到的会是 NotInitialized 而不是 OutOfRange。
    my::net::TcpSocket s;
    CHECK(!s.valid());

    const std::size_t too_big = 257u * 1024u * 1024u;   // 超过 kMaxBufferBytes（256 MiB）
    auto              r       = s.recv_exact(too_big);
    CHECK(!r);
    CHECK_EQ(r.status().code(), my::Errc::OutOfRange);

    const std::string msg = r.status().message();
    CHECK(msg.find(std::to_string(too_big)) != std::string::npos);   // 报出请求的字节数
    CHECK(msg.find("268435456") != std::string::npos);               // 报出上限 256 MiB
}

MYTEST_CASE(net_recv_exact_partial_then_close) {
    auto l = my::net::TcpListener::bind({"127.0.0.1", 0}, 1);
    CHECK_RESULT(l);

    const std::uint8_t three[3] = {'a', 'b', 'c'};
    std::atomic<bool>  sent{false};
    JoinGuard          peer(std::thread([&] {
        auto c = l.value().accept(3000);
        if (!c) return;
        auto st = c.value().send_all(three, sizeof(three));
        sent    = st.ok();
        // 半关闭写端：3 字节保证送达，对端随后读到 EOF（比直接 close 更确定，
        // 不会因为接收队列里有数据而变成 RST）
        if (st) (void)c.value().shutdown_write();
        std::this_thread::sleep_for(20ms);
        c.value().close();
    }));

    auto cli = my::net::TcpSocket::connect({"127.0.0.1", l.value().port()}, 3000);
    CHECK_RESULT(cli);
    CHECK_OK(cli.value().set_timeout(2000));
    if (peer.t.joinable()) peer.t.join();
    CHECK(sent.load());

    auto got = cli.value().recv_exact(64);
    CHECK(!got);
    CHECK_EQ(got.status().code(), my::Errc::IoError);
    // 现场定位"只收到半包"全靠这个消息里的数字
    CHECK(got.status().message().find("已收 3/64") != std::string::npos);
    CHECK(cli.value().valid());   // recv_exact 失败不隐式关闭句柄
}

MYTEST_CASE(net_bind_port_in_use) {
    auto first = my::net::TcpListener::bind({"127.0.0.1", 0}, 4);
    CHECK_RESULT(first);
    const std::uint16_t port = first.value().port();
    CHECK(port != 0);

    // 第二个 listener 抢同一端口。显式 reuse_addr=false：SO_REUSEADDR 打开时
    // Windows 允许重复 bind 同一地址，测的就不是"端口被占用"了。
    auto second = my::net::TcpListener::bind({"127.0.0.1", port}, 4, /*reuse_addr=*/false);
    CHECK(!second);
    CHECK_EQ(second.status().code(), my::Errc::IoError);
    CHECK(second.status().message().find("bind 失败") != std::string::npos);

    // 失败不能影响原来那个监听者
    CHECK(first.value().valid());
    CHECK_EQ(first.value().port(), port);
}

MYTEST_CASE(net_send_all_large_buffer) {
    auto p = SocketPair::make();
    CHECK_RESULT(p);

    const std::size_t total = 1024u * 1024u;   // 1 MiB：远超内核单次发送缓冲，必然部分写
    my::net::Bytes    payload(total);
    for (std::size_t i = 0; i < total; ++i) {
        payload[i] = static_cast<std::uint8_t>((i * 31u + 7u) & 0xFFu);
    }

    std::atomic<int> send_code{static_cast<int>(my::Errc::Ok)};
    JoinGuard        sender(std::thread([&] {
        auto st = p.value().a.send_all(payload.data(), payload.size());
        send_code.store(static_cast<int>(st ? my::Errc::Ok : st.code()));
    }));

    auto got = p.value().b.recv_exact(total);
    CHECK_RESULT(got);
    CHECK_EQ(got.value().size(), total);
    CHECK(got.value() == payload);   // 逐字节一致：部分写之后循环发送没有丢/错位

    if (sender.t.joinable()) sender.t.join();
    CHECK_EQ(send_code.load(), static_cast<int>(my::Errc::Ok));
}

MYTEST_CASE(net_move_transfers_handle) {
    auto p = SocketPair::make();
    CHECK_RESULT(p);

    const std::intptr_t handle = p.value().a.native_handle();
    CHECK(p.value().a.valid());
    CHECK(p.value().b.valid());
    CHECK(handle != static_cast<std::intptr_t>(-1));

    // move 构造：句柄转移到新对象，源对象必须失效
    my::net::TcpSocket moved(std::move(p.value().a));
    CHECK(!p.value().a.valid());
    CHECK(moved.valid());
    CHECK_EQ(moved.native_handle(), handle);
    CHECK(p.value().a.native_handle() != handle);

    // 转移的是同一个连接，而不只是标志位：写进去，对端能收到
    const std::string ping = "moved-handle";
    CHECK_OK(moved.send_all(ping.data(), ping.size()));
    auto got = p.value().b.recv_exact(ping.size());
    CHECK_RESULT(got);
    CHECK_EQ(std::string(got.value().begin(), got.value().end()), ping);

    // move 赋值：目标拿到句柄，源对象失效
    auto p2 = SocketPair::make();
    CHECK_RESULT(p2);
    const std::intptr_t handle2 = p2.value().b.native_handle();
    CHECK(handle2 != static_cast<std::intptr_t>(-1));

    my::net::TcpSocket target;
    target = std::move(p2.value().b);
    CHECK(!p2.value().b.valid());
    CHECK(target.valid());
    CHECK_EQ(target.native_handle(), handle2);
    CHECK(p2.value().b.native_handle() != handle2);

    // 覆盖一个已经持有句柄的目标：旧句柄必须被关闭（不能泄漏）
    my::net::TcpSocket holder;
    holder = std::move(p2.value().a);   // 先接管 p2 的 client 端
    CHECK(holder.valid());
    holder = std::move(target);         // 旧句柄在此被 close，改持 handle2
    CHECK(!target.valid());
    CHECK(holder.valid());
    CHECK_EQ(holder.native_handle(), handle2);

    // 旧句柄确实被关掉了：它的对端正是 handle2 这条连接，现在读到 EOF
    std::uint8_t buf[8];
    auto         eof = holder.recv(buf, sizeof(buf));
    CHECK_RESULT(eof);
    CHECK_EQ(eof.value(), std::size_t{0});
}

MYTEST_CASE(net_read_line_crlf) {
    auto p = SocketPair::make();
    CHECK_RESULT(p);

    // 全 CRLF 行尾 + 一个空行 + 最后一行没有换行符（对端完全关闭，不是半关闭）
    const std::string text = "abc\r\n\r\ntail";
    CHECK_OK(p.value().a.send_all(text.data(), text.size()));
    p.value().a.close();

    std::string line;
    CHECK_OK(my::net::read_line(p.value().b, line));
    CHECK_EQ(line, std::string("abc"));   // 行尾 '\r' 必须去掉
    CHECK(line.find('\r') == std::string::npos);

    CHECK_OK(my::net::read_line(p.value().b, line));
    CHECK(line.empty());                  // 空行是合法的一行

    CHECK_OK(my::net::read_line(p.value().b, line));
    CHECK_EQ(line, std::string("tail"));  // 没有换行符的最后一行靠 EOF 收尾
    CHECK(line.find('\r') == std::string::npos);

    // 剩余部分已经消费完，再读就是"对端关闭且没读到行"
    auto st = my::net::read_line(p.value().b, line);
    CHECK(!st);
    CHECK_EQ(st.code(), my::Errc::IoError);
}

MYTEST_CASE(net_endpoint_parse_errors) {
    // 非法端口必须报错，而不是退化成 0 端口静默通过
    auto bad_port = my::net::parse_endpoint("host:notaport");
    CHECK(!bad_port);
    CHECK_EQ(bad_port.status().code(), my::Errc::ParseError);
    CHECK(bad_port.status().message().find("端口不是数字") != std::string::npos);

    auto too_big = my::net::parse_endpoint("host:70000");
    CHECK(!too_big);
    CHECK_EQ(too_big.status().code(), my::Errc::OutOfRange);

    auto just_big = my::net::parse_endpoint("host:65536");   // 刚越过 65535
    CHECK(!just_big);
    CHECK_EQ(just_big.status().code(), my::Errc::OutOfRange);

    // 边界值本身合法，说明上面拒的是越界而不是"数字太大就拒"
    CHECK_OK_VALUE(max_port, my::net::parse_endpoint("host:65535"));
    CHECK_EQ(max_port.port, std::uint16_t{65535});
    CHECK(max_port.host == "host");

    // IPv6 缺 ']' / ']' 后面不是 ':'
    auto no_bracket = my::net::parse_endpoint("[::1");
    CHECK(!no_bracket);
    CHECK_EQ(no_bracket.status().code(), my::Errc::InvalidArgument);

    auto bad_tail = my::net::parse_endpoint("[::1]x");
    CHECK(!bad_tail);
    CHECK_EQ(bad_tail.status().code(), my::Errc::InvalidArgument);
}
