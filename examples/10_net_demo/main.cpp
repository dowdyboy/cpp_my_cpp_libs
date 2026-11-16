// =============================================================================
//  example_net —— TCP / UDP 的基本用法（全程回环，不依赖外部网络）
//
//  演示三件事：
//    1. TCP 回显：一个后台线程当服务端，主线程当客户端，收发一条消息；
//    2. 长度前缀分帧：板端 <-> 上位机自定义协议最常用的格式；
//    3. UDP 往返：遥测/发现常用，顺便展示广播开关。
//
//  真实部署时把 "127.0.0.1" 换成对端 IP 即可；服务端监听 "0.0.0.0"。
// =============================================================================

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "my_cpp_libs/log/format.hpp"
#include "my_cpp_libs/log/log.hpp"
#include "my_cpp_libs/net/net.hpp"

namespace {

using namespace std::chrono_literals;

/// 后台跑一个"读什么回什么"的 TCP 服务端
void run_echo_server(my::net::TcpListener& listener, int expected_conns) {
    for (int i = 0; i < expected_conns; ++i) {
        auto c = listener.accept(3000);
        if (!c) {
            my::log::warn("accept 失败: {}", c.status().to_string());
            return;
        }
        std::uint8_t buf[4096];
        while (true) {
            auto r = c.value().recv(buf, sizeof(buf));
            if (!r || r.value() == 0) break;
            if (!c.value().send_all(buf, r.value())) break;
        }
    }
}

}  // namespace

int main() {
    my::log::Config cfg;
    cfg.level      = my::log::Level::Info;
    cfg.to_console = true;
    my::log::init(cfg);

    // -------------------------------------------------------------------------
    // 1. TCP 回显
    // -------------------------------------------------------------------------
    auto listener = my::net::TcpListener::bind({"127.0.0.1", 0}, 4);
    if (!listener) {
        my::log::error("监听失败: {}", listener.status().to_string());
        return 1;
    }
    const std::uint16_t port = listener.value().port();
    my::log::info("TCP 服务端监听 127.0.0.1:{}", port);

    std::thread server_thread([&] { run_echo_server(listener.value(), 1); });

    auto client = my::net::TcpSocket::connect({"127.0.0.1", port}, 3000);
    if (!client) {
        my::log::error("连接失败: {}", client.status().to_string());
        return 1;
    }
    client.value().set_timeout(3000);
    my::log::info("已连接，对端 {}", client.value().peer().to_string());

    const std::string msg = "hello from my::net";
    if (auto st = client.value().send_all(msg.data(), msg.size()); !st) {
        my::log::error("发送失败: {}", st.to_string());
        return 1;
    }
    auto echoed = client.value().recv_exact(msg.size());
    if (!echoed) {
        my::log::error("接收失败: {}", echoed.status().to_string());
        return 1;
    }
    my::log::info("回显: {}", std::string(echoed.value().begin(), echoed.value().end()));

    // -------------------------------------------------------------------------
    // 2. 分帧协议
    // -------------------------------------------------------------------------
    const std::string frame = R"({"cmd":"set_gain","value":12.5})";
    if (auto st = my::net::write_frame(client.value(), frame); !st) {
        my::log::error("写帧失败: {}", st.to_string());
        return 1;
    }
    auto got = my::net::read_frame(client.value(), 4096);
    if (!got) {
        my::log::error("读帧失败: {}", got.status().to_string());
        return 1;
    }
    my::log::info("帧往返 {} 字节: {}", got.value().size(),
                  std::string(got.value().begin(), got.value().end()));

    client.value().close();
    if (server_thread.joinable()) server_thread.join();
    listener.value().close();

    // -------------------------------------------------------------------------
    // 3. UDP
    // -------------------------------------------------------------------------
    auto rx = my::net::UdpSocket::bind({"127.0.0.1", 0});
    auto tx = my::net::UdpSocket::open();
    if (!rx || !tx) {
        my::log::error("UDP 初始化失败");
        return 1;
    }
    rx.value().set_timeout(2000);
    if (auto st = tx.value().set_broadcast(true); !st) {
        my::log::warn("开广播失败（有些环境不允许）: {}", st.to_string());
    }
    const std::string telemetry = "temp=36.5;vib=0.02";
    auto              sent = tx.value().send_to({"127.0.0.1", rx.value().port()}, telemetry.data(),
                                                telemetry.size());
    if (!sent) {
        my::log::error("UDP 发送失败: {}", sent.status().to_string());
        return 1;
    }
    auto d = rx.value().recv_from();
    if (!d) {
        my::log::error("UDP 接收失败: {}", d.status().to_string());
        return 1;
    }
    my::log::info("UDP 收到 {} 字节，来自 {}: {}", d.value().data.size(),
                  d.value().from.host, d.value().from.port);
    my::log::info("内容: {}", std::string(d.value().data.begin(), d.value().data.end()));

    my::log::shutdown();
    return 0;
}
