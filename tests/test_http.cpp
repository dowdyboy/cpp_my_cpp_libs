// =============================================================================
//  my::http 单元测试
//
//  不依赖外部服务：每个用例自己起一个 Server（绑 127.0.0.1:0），再用 Client 打。
//  覆盖：路由、JSON 往返、404/错误体、自定义 header、超时、连接失败、stop。
// =============================================================================

#include "my_cpp_libs/http/http.hpp"

#include <chrono>
#include <string>
#include <thread>

#include "test_framework.hpp"

MYTEST_SUITE("http");

namespace {

using namespace std::chrono_literals;

my::http::ClientOptions client_opts(const std::string& base, int read_ms = 5000) {
    my::http::ClientOptions o;
    o.base_url        = base;
    o.connect_timeout_ms = 2000;
    o.read_timeout_ms = read_ms;
    return o;
}

struct TestServer {
    my::http::Server server;

    static my::Result<TestServer> make() {
        my::http::ServerOptions opt;
        opt.host = "127.0.0.1";
        opt.port = 0;  // 系统分配
        auto s   = my::http::Server::create(opt);
        if (!s) return s.status();
        TestServer ts;
        ts.server = std::move(s.value());
        return ts;
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(server.port());
    }
};

}  // namespace

// -----------------------------------------------------------------------------
//  基础往返
// -----------------------------------------------------------------------------
MYTEST_CASE(http_get_and_post) {
    auto ts = TestServer::make();
    CHECK_RESULT(ts);

    ts.value().server.get("/ping", [](const my::http::Request&, my::http::Response& res) {
        res.set_content("pong");
    });
    ts.value().server.post("/echo", [](const my::http::Request& req, my::http::Response& res) {
        res.set_content(req.body, "application/octet-stream");
    });

    CHECK_OK(ts.value().server.listen_async());

    auto cli = my::http::Client::create(client_opts(ts.value().base_url()));
    CHECK_RESULT(cli);

    auto ping = cli.value().get("/ping");
    CHECK_RESULT(ping);
    CHECK_EQ(ping.value().status, 200);
    CHECK(ping.value().body == "pong");

    auto echo = cli.value().post("/echo", "hello-raw", "text/plain");
    CHECK_RESULT(echo);
    CHECK_EQ(echo.value().status, 200);
    CHECK(echo.value().body == "hello-raw");

    ts.value().server.stop();
}

MYTEST_CASE(http_json_roundtrip) {
    auto ts = TestServer::make();
    CHECK_RESULT(ts);

    ts.value().server.post("/api/echo", [](const my::http::Request& req,
                                            my::http::Response& res) {
        auto doc = req.json();
        if (!doc) {
            res.set_error(400, doc.status().to_string());
            return;
        }
        my::json::Document out;
        out.set("topic", doc.value().get_or<std::string>("topic", "?"));
        out.set("value", doc.value().get_or<double>("value", 0.0));
        out.set("ok", true);
        res.set_json(out);
    });

    CHECK_OK(ts.value().server.listen_async());

    auto cli = my::http::Client::create(client_opts(ts.value().base_url()));
    CHECK_RESULT(cli);

    my::json::Document req;
    req.set("topic", "sensor/temp");
    req.set("value", 36.5);

    auto r = cli.value().post_json("/api/echo", req);
    CHECK_RESULT(r);
    CHECK_EQ(r.value().status, 200);

    auto out = my::json::Document::parse(r.value().body);
    CHECK_RESULT(out);
    CHECK(out.value().get_or<std::string>("topic", "") == "sensor/temp");
    CHECK_NEAR(out.value().get_or<double>("value", 0.0), 36.5, 1e-9);
    CHECK(out.value().get_or<bool>("ok", false));

    ts.value().server.stop();
}

MYTEST_CASE(http_custom_request_header) {
    auto ts = TestServer::make();
    CHECK_RESULT(ts);

    ts.value().server.get("/whoami", [](const my::http::Request& req,
                                        my::http::Response& res) {
        res.set_content(req.header("x-device-id"));
    });
    CHECK_OK(ts.value().server.listen_async());

    auto cli = my::http::Client::create(client_opts(ts.value().base_url()));
    CHECK_RESULT(cli);

    my::http::Headers extra{{"X-Device-Id", "zynq-7020-01"}};
    auto               r = cli.value().request("GET", "/whoami", "", "", extra);
    CHECK_RESULT(r);
    CHECK(r.value().body == "zynq-7020-01");
    // header 名字大小写不敏感
    ts.value().server.stop();
}

MYTEST_CASE(http_404_and_error_body) {
    auto ts = TestServer::make();
    CHECK_RESULT(ts);

    ts.value().server.get("/bad", [](const my::http::Request&, my::http::Response& res) {
        res.set_error(422, "参数不合法");
    });
    CHECK_OK(ts.value().server.listen_async());

    auto cli = my::http::Client::create(client_opts(ts.value().base_url()));
    CHECK_RESULT(cli);

    // 不存在的路由：httplib 自带 404
    auto nf = cli.value().get("/nope");
    CHECK_RESULT(nf);
    CHECK_EQ(nf.value().status, 404);

    // 业务错误：状态码 + JSON 错误体都要能拿到
    auto bad = cli.value().get("/bad");
    CHECK_RESULT(bad);
    CHECK_EQ(bad.value().status, 422);
    auto body = my::json::Document::parse(bad.value().body);
    CHECK_RESULT(body);
    CHECK(body.value().get_or<std::string>("error", "") == "参数不合法");

    // get_json 对非 2xx 应当返回 RuntimeError（而不是把错误体当数据解析）
    auto gj = cli.value().get_json("/bad");
    CHECK(!gj);
    CHECK_EQ(gj.status().code(), my::Errc::RuntimeError);

    ts.value().server.stop();
}

MYTEST_CASE(http_read_timeout) {
    auto ts = TestServer::make();
    CHECK_RESULT(ts);

    ts.value().server.get("/slow", [](const my::http::Request&, my::http::Response& res) {
        std::this_thread::sleep_for(400ms);
        res.set_content("late");
    });
    CHECK_OK(ts.value().server.listen_async());

    my::http::ClientOptions opt;
    opt.base_url         = ts.value().base_url();
    opt.read_timeout_ms  = 100;
    auto cli             = my::http::Client::create(opt);
    CHECK_RESULT(cli);

    const auto t0 = std::chrono::steady_clock::now();
    auto       r  = cli.value().get("/slow");
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    CHECK(!r);
    CHECK_EQ(r.status().code(), my::Errc::Timeout);
    // 确认真的是"等到超时才返回"，而不是立刻因为别的原因失败
    CHECK(ms >= 90);

    ts.value().server.stop();
}

MYTEST_CASE(http_connect_refused) {
    // 先占一个端口再放掉，然后去连它
    std::uint16_t port = 0;
    {
        auto s = TestServer::make();
        CHECK_RESULT(s);
        port = s.value().server.port();
    }  // TestServer 析构时 stop

    my::http::ClientOptions opt;
    opt.base_url          = "http://127.0.0.1:" + std::to_string(port);
    opt.connect_timeout_ms = 300;
    auto cli              = my::http::Client::create(opt);
    CHECK_RESULT(cli);

    auto r = cli.value().get("/x");
    CHECK(!r);
    CHECK(r.status().code() == my::Errc::IoError || r.status().code() == my::Errc::Timeout);
}

MYTEST_CASE(http_stop_releases_port) {
    auto ts = TestServer::make();
    CHECK_RESULT(ts);
    ts.value().server.get("/x", [](const my::http::Request&, my::http::Response& res) {
        res.set_content("x");
    });
    CHECK_OK(ts.value().server.listen_async());

    const std::uint16_t port = ts.value().server.port();

    auto cli = my::http::Client::create(client_opts(ts.value().base_url()));
    CHECK_RESULT(cli);
    CHECK_RESULT(cli.value().get("/x"));

    ts.value().server.stop();

    // stop 之后再连应当失败（端口已释放，没人监听）
    auto cli2 = my::http::Client::create(client_opts("http://127.0.0.1:" + std::to_string(port)));
    CHECK_RESULT(cli2);
    auto r = cli2.value().get("/x");
    CHECK(!r);
}

MYTEST_CASE(http_invalid_base_url) {
    CHECK_ERR(my::http::Client::create(client_opts("")));
    CHECK_ERR(my::http::Client::create(client_opts("ftp://127.0.0.1:21")));
    CHECK_ERR(my::http::Client::create(client_opts("https://127.0.0.1:443")));  // 未编译 TLS
}

MYTEST_CASE(http_query_and_path_params) {
    auto ts = TestServer::make();
    CHECK_RESULT(ts);

    ts.value().server.get("/api/item/:id", [](const my::http::Request& req,
                                              my::http::Response& res) {
        res.set_content(req.param("id") + "|" + req.query_param("fmt") + "|" +
                        req.query_param("missing"));
    });
    CHECK_OK(ts.value().server.listen_async());

    auto cli = my::http::Client::create(client_opts(ts.value().base_url()));
    CHECK_RESULT(cli);

    auto r = cli.value().get("/api/item/42?fmt=json");
    CHECK_RESULT(r);
    CHECK(r.value().body == "42|json|");
    ts.value().server.stop();
}

MYTEST_CASE(http_route_rejects_unknown_method) {
    auto ts = TestServer::make();
    CHECK_RESULT(ts);

    auto st = ts.value().server.route("TRACE", "/x",
                                      [](const my::http::Request&, my::http::Response&) {});
    CHECK(!st);
    CHECK_EQ(st.code(), my::Errc::Unsupported);

    // 支持的方法要能注册成功
    CHECK_OK(ts.value().server.route("PATCH", "/x",
                                     [](const my::http::Request&, my::http::Response& res) {
                                         res.set_content("patched");
                                     }));
}

MYTEST_CASE(http_uninitialized_client) {
    my::http::Client c;  // 没有 create
    CHECK(!c.valid());
    auto r = c.get("/x");
    CHECK(!r);
    CHECK_EQ(r.status().code(), my::Errc::NotInitialized);
}
