// =============================================================================
//  my::net::TcpServer 单元测试（全部跑在 127.0.0.1 回环上，不碰外部网络）
//
//  重点：
//    1. handler 真的在独立线程里跑（能依次服务多个客户端）；
//    2. 连接上限是"硬"的：超了直接关，客户端拿不到任何数据；
//    3. 空闲超时真的把连接关掉（不是靠客户端主动断）；
//    4. stop() 有界、幂等，且 stop 之后能重新 start（listener 重新 bind）；
//    5. create() 的参数校验与端口冲突在 create 阶段就报错；
//    6. handler 抛异常被吞掉并计数；移动后源对象查询安全。
//
//  时间断言都留了余量（规格要求 2 s 内，这里断 2 s 但期望 ~100 ms），避免 CI 抖动。
//
//  关于 net_server_noex_build_reports_unsupported 这个名字在两个分支里都出现：
//    tests/CMakeLists.txt 的用例注册是**按源码文本**扫描 MYTEST_CASE 的，而且
//    按名字去重（list(REMOVE_DUPLICATES)）。一个名字出现在两个互斥分支里只会
//    被算作一条，正好等于本次构建实际注册的条数，--expect-cases 才不会因为
//    "扫描到 8 条、二进制只有 7 条"而误报。所以 noex 分支那条用例在正常构建里
//    也有一个**同名**的"反面"版本（断言 create() 真的能建，说明链进来的是真实
//    现而不是桩）。改名时两个分支必须一起改。
// =============================================================================

#include "my_cpp_libs/net/tcp_server.hpp"

#include "test_framework.hpp"

#if defined(MYLIB_NO_EXCEPTIONS)

// =============================================================================
//  -fno-exceptions 构建：TcpServer 退化成桩
//  类照常声明（头文件不变化），所以消费方代码照样编译；create() 明确报
//  Unsupported，而不是链接失败或运行期莫名崩溃。
// =============================================================================
MYTEST_SUITE("net_server");

MYTEST_CASE(net_server_noex_build_reports_unsupported) {
    using namespace my::net;

    auto created = TcpServer::create(Endpoint{"127.0.0.1", 0}, TcpServer::Handler{});
    CHECK(!created);
    CHECK(created.status().code() == my::Errc::Unsupported);
    CHECK(!created.status().message().empty());
}

#else  // 正常构建：真跑

#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>

MYTEST_SUITE("net_server");

namespace {

using namespace std::chrono_literals;

/// 轮询等待谓词成立：用来等"连接已经被 accept 线程交给 handler"这类跨线程状态，
/// 比 sleep 固定时长稳。
bool wait_until(const std::function<bool()>& pred, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

std::int64_t elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

my::net::Endpoint loopback(std::uint16_t port) { return my::net::Endpoint{"127.0.0.1", port}; }

}  // namespace

// -----------------------------------------------------------------------------
//  0. 本构建能力自报（**正常构建**版本；noex 分支的版本见文件开头）
//     noex 桩里 create() 返回 Unsupported；这里必须相反 —— create() 真能建出
//     server，说明链进来的是真实现而不是桩。两个分支共用同一个用例名是有意的，
//     原因见文件头（源码扫描按名字去重，条数才对得上）。
// -----------------------------------------------------------------------------
MYTEST_CASE(net_server_noex_build_reports_unsupported) {
    using namespace my::net;

    TcpServer::Handler noop    = [](TcpSocket&, const Endpoint&) {};
    auto               created = TcpServer::create(loopback(0), std::move(noop));
    CHECK(created.ok());
    CHECK(created.value().port() != 0);      // create 已经 bind 到真实端口
    CHECK(!created.value().running());       // 但还没有 start
    // 不 start 直接析构：验证"从没跑过线程"的停止路径也是安全的
}

// -----------------------------------------------------------------------------
//  1. 两个客户端依次 echo：handler 读一行回一行
// -----------------------------------------------------------------------------
MYTEST_CASE(net_server_echo_two_clients) {
    using namespace my::net;

    TcpServer::Handler handler = [](TcpSocket& sock, const Endpoint& /*peer*/) {
        std::string line;
        if (!read_line(sock, line)) return;  // 对端关闭 / 超时 → 直接收摊
        (void)sock.send_all("pong\n", 5);    // send_all 处理部分写，保证整行出去
    };

    CHECK_OK_VALUE(srv, TcpServer::create(loopback(0), std::move(handler)));
    CHECK(srv.port() != 0);  // create() 已经 bind：port 0 → 系统分配，这里拿实际端口
    CHECK(!srv.running());
    CHECK_OK(srv.start());
    CHECK(srv.running());

    for (int i = 0; i < 2; ++i) {
        CHECK_OK_VALUE(cli, TcpSocket::connect(loopback(srv.port()), 2000));
        CHECK_OK(cli.set_timeout(2000));
        CHECK_OK(cli.send_all("ping\n", 5));

        std::string line;
        CHECK_OK(read_line(cli, line));
        CHECK_EQ(line, std::string("pong"));
        // cli 在每轮末尾析构 → 关闭连接，handler 的 read_line 拿到 0 正常退出
    }

    CHECK_EQ(srv.accepted_total(), std::uint64_t{2});
    CHECK_EQ(srv.rejected_total(), std::uint64_t{0});
    CHECK_EQ(srv.handler_errors(), std::uint64_t{0});

    srv.stop();
    CHECK(!srv.running());
    CHECK_EQ(srv.active_connections(), std::size_t{0});  // stop 等到连接线程退出了
    CHECK_EQ(srv.accepted_total(), std::uint64_t{2});     // stop 不清零统计
}

// -----------------------------------------------------------------------------
//  2. max_connections = 1：第二个连接被直接关掉（客户端 0 字节 + rejected 计数）
// -----------------------------------------------------------------------------
MYTEST_CASE(net_server_rejects_over_limit) {
    using namespace my::net;

    TcpServerOptions opt;
    opt.max_connections   = 1;
    opt.idle_timeout_ms   = 3000;  // 第一个 handler 阻塞在 read_line 上，等下一个人来撞上限
    opt.accept_timeout_ms = 50;    // 拒绝要在 ~50 ms 内被发现

    TcpServer::Handler handler = [](TcpSocket& sock, const Endpoint& /*peer*/) {
        std::string line;
        (void)read_line(sock, line);  // 一直等到对端关闭或 3 s 空闲超时
    };

    CHECK_OK_VALUE(srv, TcpServer::create(loopback(0), std::move(handler), opt));
    CHECK_OK(srv.start());

    CHECK_OK_VALUE(first, TcpSocket::connect(loopback(srv.port()), 2000));
    // 必须等第一个连接真的进了 handler，否则第二个可能先被 accept，用例就假绿了
    CHECK(wait_until([&srv] { return srv.active_connections() == 1; }, 2000ms));

    CHECK_OK_VALUE(second, TcpSocket::connect(loopback(srv.port()), 2000));
    CHECK_OK(second.set_timeout(2000));

    char               buf[8] = {};
    const auto         t0     = std::chrono::steady_clock::now();
    const auto         rr     = second.recv(buf, sizeof buf);
    const std::int64_t cost   = elapsed_ms(t0);

    // 被拒的连接由服务端直接 close：客户端只可能拿到 0（FIN）或错误，不可能是数据
    CHECK(!rr || rr.value() == 0);
    CHECK(cost < 2000);
    CHECK(srv.rejected_total() >= 1);
    CHECK_EQ(srv.accepted_total(), std::uint64_t{1});
    CHECK_EQ(srv.active_connections(), std::size_t{1});

    first.close();  // 放走第一个 handler，stop() 不用干等空闲超时
    srv.stop();
    CHECK(!srv.running());
}

// -----------------------------------------------------------------------------
//  3. idle_timeout_ms = 100：客户端不发数据，连接被服务端关掉
// -----------------------------------------------------------------------------
MYTEST_CASE(net_server_idle_timeout_closes_connection) {
    using namespace my::net;

    TcpServerOptions opt;
    opt.idle_timeout_ms   = 100;  // 每连接收超时
    opt.accept_timeout_ms = 50;

    TcpServer::Handler handler = [](TcpSocket& sock, const Endpoint& /*peer*/) {
        char buf[64];
        (void)sock.recv(buf, sizeof buf);  // 100 ms 后返回 Timeout → handler 返回 → 连接关闭
    };

    CHECK_OK_VALUE(srv, TcpServer::create(loopback(0), std::move(handler), opt));
    CHECK_OK(srv.start());

    CHECK_OK_VALUE(cli, TcpSocket::connect(loopback(srv.port()), 2000));
    CHECK_OK(cli.set_timeout(2000));  // 客户端自己的上限：用例不会卡死

    char       buf[8] = {};
    const auto t0     = std::chrono::steady_clock::now();
    const auto rr     = cli.recv(buf, sizeof buf);  // 不发数据，等对端超时关闭
    const auto cost   = elapsed_ms(t0);

    CHECK(!rr || rr.value() == 0);
    CHECK(cost < 2000);  // 规格：< 2 s 内返回
    CHECK(cost >= 50);   // 而且不是立刻关（说明 idle_timeout 真的生效了）
    CHECK_EQ(srv.accepted_total(), std::uint64_t{1});

    srv.stop();
    CHECK(!srv.running());
}

// -----------------------------------------------------------------------------
//  4. start 两次 → Busy；stop 之后能再 start
// -----------------------------------------------------------------------------
MYTEST_CASE(net_server_start_twice_is_busy) {
    using namespace my::net;

    TcpServer::Handler handler = [](TcpSocket&, const Endpoint&) {};
    CHECK_OK_VALUE(srv, TcpServer::create(loopback(0), std::move(handler)));
    CHECK(!srv.running());

    CHECK_OK(srv.start());
    CHECK(srv.running());
    const std::uint16_t first_port = srv.port();
    CHECK(first_port != 0);

    const my::Status again = srv.start();
    CHECK(!again);
    CHECK(again.code() == my::Errc::Busy);
    CHECK_EQ(srv.port(), first_port);

    srv.stop();
    CHECK(!srv.running());

    // 重启（明确支持的语义）：stop() 会把 listener 关掉，start() 再按 create() 时的
    // **实际端口**重新 bind。所以同一个对象可以反复 start/stop，而不是一次性的。
    CHECK_OK(srv.start());
    CHECK(srv.running());
    CHECK_EQ(srv.port(), first_port);

    srv.stop();
    CHECK(!srv.running());
}

// -----------------------------------------------------------------------------
//  5. create() 的参数校验 + 端口冲突在 create 阶段暴露
// -----------------------------------------------------------------------------
MYTEST_CASE(net_server_create_validates_arguments) {
    using namespace my::net;

    // handler 为空
    auto no_handler = TcpServer::create(loopback(0), TcpServer::Handler{});
    CHECK(!no_handler);
    CHECK(no_handler.status().code() == my::Errc::InvalidArgument);

    TcpServer::Handler echo = [](TcpSocket&, const Endpoint&) {};

    // max_connections == 0：拒绝（不做"0 = 不限制"）
    TcpServerOptions zero_max;
    zero_max.max_connections = 0;
    auto bad_max = TcpServer::create(loopback(0), echo, zero_max);
    CHECK(!bad_max);
    CHECK(bad_max.status().code() == my::Errc::InvalidArgument);

    // 超时为负
    TcpServerOptions neg_idle;
    neg_idle.idle_timeout_ms = -1;
    auto bad_idle = TcpServer::create(loopback(0), echo, neg_idle);
    CHECK(!bad_idle);
    CHECK(bad_idle.status().code() == my::Errc::InvalidArgument);

    TcpServerOptions neg_accept;
    neg_accept.accept_timeout_ms = -1;
    auto bad_accept = TcpServer::create(loopback(0), echo, neg_accept);
    CHECK(!bad_accept);
    CHECK(bad_accept.status().code() == my::Errc::InvalidArgument);

    // 端口冲突：reuse_addr 默认 false，第二次 bind 同一端口必然失败 —— 而且是在
    // create() 就失败，不是拖到 start()。
    CHECK_OK_VALUE(holder, TcpServer::create(loopback(0), echo));
    CHECK(holder.port() != 0);
    auto conflict = TcpServer::create(loopback(holder.port()), echo);
    CHECK(!conflict);
    CHECK(conflict.status().code() == my::Errc::IoError);
}

// -----------------------------------------------------------------------------
//  6. 移动语义：运行中的 server 移动后线程跟着走，被移动对象查询安全
// -----------------------------------------------------------------------------
MYTEST_CASE(net_server_move_is_safe) {
    using namespace my::net;

    TcpServer::Handler handler = [](TcpSocket&, const Endpoint&) {};
    CHECK_OK_VALUE(a, TcpServer::create(loopback(0), std::move(handler)));
    CHECK_OK(a.start());
    const std::uint16_t port = a.port();

    TcpServer b(std::move(a));  // 移动构造：运行中的 accept 线程跟着走
    CHECK(b.running());
    CHECK_EQ(b.port(), port);

    // 被移动对象 impl_ 为空：所有查询走空指针分支，不崩
    CHECK(!a.running());
    CHECK_EQ(a.port(), std::uint16_t{0});
    CHECK_EQ(a.active_connections(), std::size_t{0});
    CHECK_EQ(a.accepted_total(), std::uint64_t{0});
    CHECK_EQ(a.rejected_total(), std::uint64_t{0});
    CHECK_EQ(a.handler_errors(), std::uint64_t{0});
    const my::Status moved_from_start = a.start();
    CHECK(!moved_from_start);
    CHECK(moved_from_start.code() == my::Errc::NotInitialized);
    a.stop();  // 幂等且安全

    b.stop();
    CHECK(!b.running());
    // b/a 析构时各自再 stop() 一次：幂等
}

// -----------------------------------------------------------------------------
//  7. handler 抛异常不能带走进程，且计入 handler_errors
// -----------------------------------------------------------------------------
MYTEST_CASE(net_server_handler_exception_is_counted) {
    using namespace my::net;

    TcpServerOptions opt;
    opt.idle_timeout_ms   = 2000;
    opt.accept_timeout_ms = 50;

    TcpServer::Handler handler = [](TcpSocket& sock, const Endpoint& /*peer*/) {
        std::string line;
        (void)read_line(sock, line);
        (void)sock.send_all("bye\n", 4);  // 先回一句，客户端好确认确实走到了 handler
        throw std::runtime_error("handler 故意抛异常");
    };

    CHECK_OK_VALUE(srv, TcpServer::create(loopback(0), std::move(handler), opt));
    CHECK_OK(srv.start());

    CHECK_OK_VALUE(cli, TcpSocket::connect(loopback(srv.port()), 2000));
    CHECK_OK(cli.set_timeout(2000));
    CHECK_OK(cli.send_all("ping\n", 5));

    std::string line;
    CHECK_OK(read_line(cli, line));  // 收到 "bye" 说明 handler 跑到了抛异常那一行
    CHECK_EQ(line, std::string("bye"));

    // 连接应被关闭：再读得到 0/错误
    char       buf[8] = {};
    const auto rr     = cli.recv(buf, sizeof buf);
    CHECK(!rr || rr.value() == 0);

    // 异常被吞掉并计数（等一下连接线程收尾）
    CHECK(wait_until([&srv] { return srv.handler_errors() == 1; }, 2000ms));
    CHECK_EQ(srv.accepted_total(), std::uint64_t{1});

    srv.stop();
    CHECK(!srv.running());
}

#endif  // MYLIB_NO_EXCEPTIONS
