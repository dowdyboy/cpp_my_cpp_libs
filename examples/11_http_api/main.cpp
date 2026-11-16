// =============================================================================
//  example_http —— 板端 REST + 客户端调用
//
//  两个方向都演示：
//    1. 板上跑一个 HTTP 服务端（/status 返回 JSON、/echo 回显 POST 的 JSON）；
//    2. 用 my::http::Client 调它；如果给了命令行参数，就把同一份 JSON 发给
//       参数指定的地址（比如你上位机的 Flask：http://192.168.1.50:5000/api/upload）。
//
//  运行：
//    example_http                              # 自测（回环）
//    example_http http://192.168.1.50:5000/api/upload   # 顺带发给 Flask
// =============================================================================

#include <chrono>
#include <string>
#include <thread>

#include "my_cpp_libs/http/http.hpp"
#include "my_cpp_libs/log/format.hpp"
#include "my_cpp_libs/log/log.hpp"

namespace {

using namespace std::chrono_literals;

/// 板端要上报的一份数据
my::json::Document make_telemetry() {
    my::json::Document doc;
    doc.set("device", "zynq-7020");
    doc.set("temp_c", 36.5);
    doc.set("vib", 0.021);
    doc.set("ts", 1720000000);
    return doc;
}

}  // namespace

int main(int argc, char** argv) {
    my::log::Config cfg;
    cfg.level      = my::log::Level::Info;
    cfg.to_console = true;
    my::log::init(cfg);

    // -------------------------------------------------------------------------
    // 1. 板端 HTTP 服务端
    // -------------------------------------------------------------------------
    my::http::ServerOptions sopt;
    sopt.host = "127.0.0.1";
    sopt.port = 0;  // 系统分配，避免和别的服务撞端口

    auto server = my::http::Server::create(sopt);
    if (!server) {
        my::log::error("HTTP 服务端创建失败: {}", server.status().to_string());
        return 1;
    }

    server.value().get("/status", [](const my::http::Request&, my::http::Response& res) {
        my::json::Document doc;
        doc.set("ok", true);
        doc.set("uptime_s", 1234);
        doc.set("backend", "my::http");
        res.set_json(doc);
    });

    server.value().post("/echo", [](const my::http::Request& req, my::http::Response& res) {
        auto doc = req.json();
        if (!doc) {
            res.set_error(400, "请求体不是合法 JSON");
            return;
        }
        my::json::Document out;
        out.set("received", doc.value().get_or<std::string>("device", "?"));
        out.set("value", doc.value().get_or<double>("temp_c", 0.0));
        res.set_json(out);
    });

    if (auto st = server.value().listen_async(); !st) {
        my::log::error("HTTP 启动失败: {}", st.to_string());
        return 1;
    }
    const std::string base = "http://127.0.0.1:" + std::to_string(server.value().port());
    my::log::info("板端 HTTP 服务: {}/status  {}/echo", base, base);

    // -------------------------------------------------------------------------
    // 2. 客户端调用（GET /status，POST /echo）
    // -------------------------------------------------------------------------
    my::http::ClientOptions copt;
    copt.base_url         = base;
    copt.connect_timeout_ms = 2000;
    copt.read_timeout_ms  = 3000;

    auto client = my::http::Client::create(copt);
    if (!client) {
        my::log::error("客户端创建失败: {}", client.status().to_string());
        return 1;
    }

    auto status = client.value().get_json("/status");
    if (!status) {
        my::log::error("GET /status 失败: {}", status.status().to_string());
    } else {
        my::log::info("GET /status -> ok={} uptime={}s",
                      status.value().get_or<bool>("ok", false),
                      status.value().get_or<int>("uptime_s", 0));
    }

    const auto payload = make_telemetry();
    auto       echoed  = client.value().post_json("/echo", payload);
    if (!echoed) {
        my::log::error("POST /echo 失败: {}", echoed.status().to_string());
    } else {
        my::log::info("POST /echo -> {}", echoed.value().body);
    }

    // -------------------------------------------------------------------------
    // 3. 可选：把这包数据发给外部服务（例如上位机的 Flask）
    // -------------------------------------------------------------------------
    if (argc > 1) {
        const std::string url = argv[1];
        const auto        pos = url.find("/", url.find("//") + 2);
        const std::string base_url = pos == std::string::npos ? url : url.substr(0, pos);
        const std::string path     = pos == std::string::npos ? "/" : url.substr(pos);

        my::http::ClientOptions o;
        o.base_url        = base_url;
        o.read_timeout_ms = 5000;
        auto up = my::http::Client::create(o);
        if (!up) {
            my::log::error("外部客户端创建失败: {}", up.status().to_string());
        } else {
            auto r = up.value().post_json(path, payload);
            if (!r) {
                my::log::error("上报到 {} 失败: {}", url, r.status().to_string());
            } else {
                my::log::info("上报到 {} -> HTTP {}，{} 字节", url, r.value().status,
                              r.value().body.size());
            }
        }
    }

    // 给对端一点时间收尾，再停服务
    std::this_thread::sleep_for(100ms);
    server.value().stop();
    my::log::shutdown();
    return 0;
}
