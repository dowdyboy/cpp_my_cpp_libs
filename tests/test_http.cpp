// =============================================================================
//  my::http 单元测试
//
//  不依赖外部服务：每个用例自己起一个 Server（绑 127.0.0.1:0），再用 Client 打。
//  覆盖：路由、JSON 往返、404/错误体、自定义 header、超时、连接失败、stop。
// =============================================================================

#include "my_cpp_libs/http/http.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
        CHECK_OK(out.set("topic", doc.value().get_or<std::string>("topic", "?")));
        CHECK_OK(out.set("value", doc.value().get_or<double>("value", 0.0)));
        CHECK_OK(out.set("ok", true));
        res.set_json(out);
    });

    CHECK_OK(ts.value().server.listen_async());

    auto cli = my::http::Client::create(client_opts(ts.value().base_url()));
    CHECK_RESULT(cli);

    my::json::Document req;
    CHECK_OK(req.set("topic", "sensor/temp"));
    CHECK_OK(req.set("value", 36.5));

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

// -----------------------------------------------------------------------------
//  base_url 解析 / Server 生命周期 / 并发
// -----------------------------------------------------------------------------

MYTEST_CASE(http_base_url_ipv6_port_parsed) {
    // 纯解析层回归：旧实现"只要出现过 ']' 就跳过端口解析"，
    // 把 "http://[::1]:8080" 整个当成主机名 + 默认端口 80。
    auto parsed = my::http::Client::create(client_opts("http://[::1]:8080"));
    CHECK_RESULT(parsed);

    // 没有 IPv6 回环的环境无法端到端验证"端口真的被采用"
    my::http::ServerOptions v6opt;
    v6opt.host = "::1";
    v6opt.port = 0;
    auto v6srv = my::http::Server::create(v6opt);
    if (!v6srv) MYTEST_SKIP("本机没有可用的 IPv6 回环（::1）");

    v6srv.value().get("/who", [](const my::http::Request&, my::http::Response& res) {
        res.set_content("v6");
    });
    CHECK_OK(v6srv.value().listen_async());
    const std::uint16_t port6 = v6srv.value().port();
    CHECK(port6 != 0);

    // IPv6 字面量 + 显式端口必须真的连到该端口（旧实现会去连 80）
    auto cli6 = my::http::Client::create(client_opts("http://[::1]:" + std::to_string(port6)));
    CHECK_RESULT(cli6);
    auto r6 = cli6.value().get("/who");
    CHECK_RESULT(r6);
    CHECK_EQ(r6.value().status, 200);
    CHECK(r6.value().body == "v6");
    v6srv.value().stop();

    // 只监听 IPv4 的服务端：同端口用 IPv4 能通，用 IPv6 字面量必须是连接错误，
    // 而不是"把端口忽略掉后连到别处"。
    auto ts = TestServer::make();
    CHECK_RESULT(ts);
    ts.value().server.get("/who", [](const my::http::Request&, my::http::Response& res) {
        res.set_content("v4");
    });
    CHECK_OK(ts.value().server.listen_async());
    const std::uint16_t port4 = ts.value().server.port();

    auto cli4 = my::http::Client::create(client_opts("http://127.0.0.1:" + std::to_string(port4)));
    CHECK_RESULT(cli4);
    auto r4 = cli4.value().get("/who");
    CHECK_RESULT(r4);
    CHECK_EQ(r4.value().status, 200);
    CHECK(r4.value().body == "v4");

    // 本机实测：连一个没人监听的 ::1 端口会走完 connect 超时才失败（不是立刻
    // refused），所以这里必须用短超时，否则单个用例会超过 2 秒预算。
    my::http::ClientOptions v6b_opts = client_opts("http://[::1]:" + std::to_string(port4));
    v6b_opts.connect_timeout_ms      = 300;
    auto cli6b = my::http::Client::create(v6b_opts);
    CHECK_RESULT(cli6b);
    auto r6b = cli6b.value().get("/who");
    CHECK(!r6b);
    CHECK(r6b.status().code() == my::Errc::IoError || r6b.status().code() == my::Errc::Timeout);
    ts.value().server.stop();
}

MYTEST_CASE(http_base_url_rejects_bad_port) {
    // 端口必须是纯数字：std::stoi 会把 "8080abc" 收成 8080（旧实现静默接受）
    auto alpha = my::http::Client::create(client_opts("http://127.0.0.1:8080abc"));
    CHECK(!alpha);
    CHECK_EQ(alpha.status().code(), my::Errc::ParseError);

    // 冒号后没有端口
    auto empty = my::http::Client::create(client_opts("http://127.0.0.1:"));
    CHECK(!empty);
    CHECK_EQ(empty.status().code(), my::Errc::ParseError);

    // IPv6 字面量缺少 ']'
    auto bracket = my::http::Client::create(client_opts("http://[::1"));
    CHECK(!bracket);
    CHECK_EQ(bracket.status().code(), my::Errc::InvalidArgument);

    // 端口越界
    auto huge = my::http::Client::create(client_opts("http://127.0.0.1:70000"));
    CHECK(!huge);
    CHECK_EQ(huge.status().code(), my::Errc::OutOfRange);
}

MYTEST_CASE(http_stop_from_handler_is_safe) {
    // 回归守门：在 handler 里调用 stop()。
    //
    // httplib 0.66 的 handler 跑在服务端**线程池的 worker** 上（默认 4 个），
    // 不是监听线程；而监听线程退出时又会 join 所有 worker。所以"从 handler 里
    // join 监听线程"必然互锁（0.1.1 之前实测死锁）。
    // 现在 stop() 在 handler 上下文里只置停止标志 + detach，不再 join。
    auto made = TestServer::make();
    CHECK_RESULT(made);
    auto              ts  = std::make_unique<TestServer>(std::move(made.value()));
    my::http::Server* srv = &ts->server;

    std::atomic<int> handler_ran{0};
    auto             reg = srv->route(
        "GET", "/stop",
        [srv, &handler_ran](const my::http::Request&, my::http::Response& res) {
            res.set_content("bye");
            handler_ran.fetch_add(1);
            srv->stop();  // handler 线程（线程池 worker）里停服务
        });
    CHECK_OK(reg);
    CHECK_OK(srv->listen_async());

    auto cli = my::http::Client::create(client_opts(ts->base_url(), /*read_ms=*/500));
    CHECK_RESULT(cli);
    (void)cli.value().get("/stop");  // 响应能否写回取决于时序，这里只守"能停且不崩"

    // handler 必须真的跑过，否则这个用例什么也没验证
    CHECK_EQ(handler_ran.load(), 1);

    // 等监听线程退出：detach 路径下 running 由监听线程在退出时自己置 false，
    // 所以 running()==false 就是"可以安全析构 Server"的信号。
    bool       stopped  = false;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!srv->running()) {
            stopped = true;
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    CHECK(stopped);  // 停不下来就是又互锁了
    CHECK(!srv->running());
    // 再调一次 stop() 也必须安全（线程已 detach/不可 join）
    srv->stop();
}

MYTEST_CASE(http_double_stop_is_safe) {
    // create 之后连续 stop() 两次
    auto ts = TestServer::make();
    CHECK_RESULT(ts);
    ts.value().server.stop();
    ts.value().server.stop();
    CHECK(!ts.value().server.running());

    // 从未 listen_async 就 stop()
    my::http::Server never;
    never.stop();
    never.stop();
    CHECK_EQ(never.port(), 0);
    CHECK(!never.running());

    // listen_async 之后再连续 stop 两次（第二次看到的是已 join 的线程）
    auto ts2 = TestServer::make();
    CHECK_RESULT(ts2);
    CHECK_OK(ts2.value().server.listen_async());
    ts2.value().server.stop();
    ts2.value().server.stop();
    CHECK(!ts2.value().server.running());
}

MYTEST_CASE(http_concurrent_requests) {
    auto ts = TestServer::make();
    CHECK_RESULT(ts);

    // 用路径参数回显，body 与请求一一对应：任何串包/串连接都会让 body 不匹配
    ts.value().server.get("/echo/:id", [](const my::http::Request& req,
                                          my::http::Response&      res) {
        res.set_content(req.param("id"));
    });
    CHECK_OK(ts.value().server.listen_async());

    const std::string base      = ts.value().base_url();
    constexpr int     kThreads  = 8;
    constexpr int     kPerThread = 10;

    std::atomic<int>         failures{0};
    std::mutex               err_mutex;
    std::vector<std::string> errors;

    auto record = [&](const std::string& msg) {
        failures.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(err_mutex);
        if (errors.size() < 8) errors.push_back(msg);
    };

    auto worker = [&](int tid) {
        // Client 不是线程安全的：一个线程一个 Client（组件约定）
        auto cli = my::http::Client::create(client_opts(base, 2000));
        if (!cli) {
            record("client[" + std::to_string(tid) + "]: " + cli.status().to_string());
            return;
        }
        for (int i = 0; i < kPerThread; ++i) {
            const std::string id = std::to_string(tid) + "-" + std::to_string(i);
            auto              r  = cli.value().get("/echo/" + id);
            if (!r) {
                record("GET " + id + ": " + r.status().to_string());
                return;
            }
            if (r.value().status != 200) {
                record("GET " + id + ": HTTP " + std::to_string(r.value().status));
                return;
            }
            if (r.value().body != id) {
                record("GET " + id + ": body=" + r.value().body);
                return;
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();

    ts.value().server.stop();

    for (const auto& e : errors) std::printf("         并发请求失败: %s\n", e.c_str());
    CHECK_EQ(failures.load(), 0);
}

MYTEST_CASE(http_uninitialized_route_reports_error) {
    my::http::Server server;  // 默认构造 = 未 create

    // Status 版 route()：把 NotInitialized 明确返回给调用方
    auto st = server.route("GET", "/x",
                           [](const my::http::Request&, my::http::Response&) {});
    CHECK(!st);
    CHECK_EQ(st.code(), my::Errc::NotInitialized);

    auto st_post = server.route("POST", "/x",
                                [](const my::http::Request&, my::http::Response&) {});
    CHECK(!st_post);
    CHECK_EQ(st_post.code(), my::Errc::NotInitialized);

    // void 便捷重载没有返回值，只能走 stderr 上报；这里验证它不崩且没有副作用
    server.get("/y", [](const my::http::Request&, my::http::Response&) {});
    server.post("/y", [](const my::http::Request&, my::http::Response&) {});
    CHECK_EQ(server.port(), 0);
    CHECK(!server.running());

    // 未 create 的 Server 上 listen_async 也必须报 NotInitialized
    auto listen = server.listen_async();
    CHECK(!listen);
    CHECK_EQ(listen.code(), my::Errc::NotInitialized);
}

// -----------------------------------------------------------------------------
//  0.1.9 补齐：PUT / DELETE 路由、静态目录挂载、请求体超限
//  （以前这几条路径一条都没测过）
// -----------------------------------------------------------------------------
MYTEST_CASE(http_put_and_delete_routes) {
    auto ts = TestServer::make();
    CHECK_RESULT(ts);

    std::string stored;
    ts.value().server.put("/item", [&stored](const my::http::Request& req, my::http::Response& res) {
        stored = req.body;
        res.set_content("put-ok");
    });
    ts.value().server.del("/item", [&stored](const my::http::Request&, my::http::Response& res) {
        stored.clear();
        res.set_content("del-ok");
    });
    CHECK_OK(ts.value().server.listen_async());

    auto cli = my::http::Client::create(client_opts(ts.value().base_url()));
    CHECK_RESULT(cli);

    auto put = cli.value().put("/item", "payload-123");
    CHECK_RESULT(put);
    CHECK_EQ(put.value().status, 200);
    CHECK_EQ(put.value().body, std::string("put-ok"));
    CHECK_EQ(stored, std::string("payload-123"));   // 服务端真的收到了 body

    auto del = cli.value().del("/item");
    CHECK_RESULT(del);
    CHECK_EQ(del.value().status, 200);
    CHECK_EQ(del.value().body, std::string("del-ok"));
    CHECK(stored.empty());                          // handler 真的被执行了

    ts.value().server.stop();
}

MYTEST_CASE(http_mount_point_serves_static_file) {
    const std::string dir =
#if defined(_WIN32)
        std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "\\mylib_http_static";
#else
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/mylib_http_static";
#endif
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    {
        std::ofstream f(std::filesystem::path(dir) / "hello.txt", std::ios::binary);
        f << "static-file-content";
    }

    auto ts = TestServer::make();
    CHECK_RESULT(ts);
    CHECK_OK(ts.value().server.set_mount_point("/static", dir));
    CHECK_OK(ts.value().server.listen_async());

    auto cli = my::http::Client::create(client_opts(ts.value().base_url()));
    CHECK_RESULT(cli);
    auto res = cli.value().get("/static/hello.txt");
    CHECK_RESULT(res);
    CHECK_EQ(res.value().status, 200);
    CHECK_EQ(res.value().body, std::string("static-file-content"));

    ts.value().server.stop();
    std::filesystem::remove_all(dir, ec);
}

MYTEST_CASE(http_payload_over_limit_returns_413) {
    my::http::ServerOptions opt;
    opt.host              = "127.0.0.1";
    opt.port              = 0;
    opt.max_payload_bytes = 1024;   // 低门限，便于触发
    auto srv              = my::http::Server::create(opt);
    CHECK_RESULT(srv);

    bool handler_ran = false;
    srv.value().post("/big", [&handler_ran](const my::http::Request&, my::http::Response& res) {
        handler_ran = true;
        res.set_content("should-not-happen");
    });
    CHECK_OK(srv.value().listen_async());

    auto cli = my::http::Client::create(
        client_opts("http://127.0.0.1:" + std::to_string(srv.value().port())));
    CHECK_RESULT(cli);
    auto res = cli.value().post("/big", std::string(4096, 'x'));

    // 客户端要么拿到 413，要么连接被服务端在读之前关掉（都算"拒绝了"）
    if (res.ok()) {
        CHECK_EQ(res.value().status, 413);
    }
    CHECK(!handler_ran);   // 关键：超限请求绝不能进业务 handler

    srv.value().stop();
}