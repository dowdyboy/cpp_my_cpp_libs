// =============================================================================
//  独立消费方示例：模拟"我自己的另一个工程"如何嵌入并使用 my_cpp_libs
//
//  这个程序会真的跑一遍四个组件，任何一步失败都返回非 0 ——
//  CI 里靠它确认"add_subdirectory 嵌入"这条路是通的。
// =============================================================================

#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

// 统一入口头：只链接用得上的组件时，它只会带进那些组件的头
#include <my_cpp_libs/my_cpp_libs.hpp>

#include <my_cpp_libs/json/json.hpp>
#include <my_cpp_libs/log/format.hpp>
#include <my_cpp_libs/log/log.hpp>
#include <my_cpp_libs/math/math.hpp>
#include <my_cpp_libs/net/net.hpp>

namespace {

int failures = 0;

void expect(bool cond, const char* what) {
    if (!cond) {
        ++failures;
        std::printf("[FAIL] %s\n", what);
    } else {
        std::printf("[ OK ] %s\n", what);
    }
}

}  // namespace

int main() {
    // ---------------------------------------------------------------- log
    my::log::Config cfg;
    cfg.logger_name = "consumer";
    cfg.level       = my::log::Level::Debug;
    if (!my::log::init(cfg)) {
        std::printf("[FAIL] log init failed\n");
        return 1;
    }
    my::log::info("消费方工程启动: {}", my::BuildInfo::summary());
    my::log::debug("版本={} 平台={}", my::BuildInfo::libraryVersion(), my::BuildInfo::osName());

    // ---------------------------------------------------------------- json
    {
        my::json::Document doc;
        doc.set("project", "consumer_app");
        doc.set("deps[0]", "my_cpp_libs::log");
        doc.set("deps[1]", "my_cpp_libs::json");
        doc.set("nested.value", 42);

        auto parsed = my::json::Document::parse(doc.dump(2));
        expect(parsed.ok(), "json roundtrip");
        if (parsed) {
            expect(parsed.value().get_or<std::string>("project", "") == "consumer_app",
                   "json 字符串取值");
            expect(parsed.value().get_or<int>("nested.value", 0) == 42, "json 嵌套路径取值");
        }
    }

    // ---------------------------------------------------------------- math
    {
        my::math::MatXd a(2, 2);
        a << 3.0, 1.0, 1.0, 2.0;
        my::math::VecXd b(2);
        b << 9.0, 8.0;

        auto x = my::math::solve(a, b);
        expect(x.ok(), "math::solve 返回结果");
        if (x) {
            expect(std::fabs(x.value()(0) - 2.0) < 1e-9 && std::fabs(x.value()(1) - 3.0) < 1e-9,
                   "math::solve 数值正确");
        }
    }

    // ---------------------------------------------------------------- net
    {
        auto listener = my::net::TcpListener::bind({"127.0.0.1", 0}, 2);
        expect(listener.ok(), "net 绑定回环端口");
        if (listener) {
            const std::uint16_t port = listener.value().port();

            std::thread server([&] {
                auto c = listener.value().accept(3000);
                if (!c) return;
                std::uint8_t buf[64];
                auto         n = c.value().recv(buf, sizeof(buf));
                if (n.ok() && n.value() > 0) {
                    c.value().send_all(buf, n.value());
                }
            });

            auto cli = my::net::TcpSocket::connect({"127.0.0.1", port}, 2000);
            expect(cli.ok(), "net 连接回环端口");
            if (cli) {
                cli.value().set_timeout(2000);
                const std::string msg = "ping";
                cli.value().send_all(msg.data(), msg.size());
                auto echoed = cli.value().recv_exact(msg.size());
                expect(echoed.ok() &&
                           std::string(echoed.value().begin(), echoed.value().end()) == msg,
                       "net 回环收发");
                cli.value().close();
            }
            if (server.joinable()) server.join();
            listener.value().close();
        }
    }

    my::log::info("消费方工程结束，失败 {} 项", failures);
    my::log::shutdown();
    return failures == 0 ? 0 : 1;
}
