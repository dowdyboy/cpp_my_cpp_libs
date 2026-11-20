#include "my_cpp_libs/http/http.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include "httplib.h"

namespace my::http {
namespace {

/// 当前线程的 handler 嵌套深度。>0 表示"我们正跑在 httplib 的 handler 里"。
///
/// 为什么需要它：httplib 0.66 的 handler 由线程池（默认 4 个 worker）执行，
/// 而监听线程退出时会 `task_queue->shutdown()` **join 所有 worker**。
/// 如果 worker 里的 handler 调 Server::stop() 并去 join 监听线程，
/// 就形成"监听线程等 worker、worker 等监听线程"的循环等待 —— 死锁（实测确认）。
/// 所以 handler 上下文里只能置停止标志 + detach，绝不能 join。
thread_local int g_handler_depth = 0;

struct HandlerScope {
    HandlerScope() noexcept { ++g_handler_depth; }
    ~HandlerScope() { --g_handler_depth; }
    HandlerScope(const HandlerScope&)            = delete;
    HandlerScope& operator=(const HandlerScope&) = delete;
};

bool in_handler_context() noexcept { return g_handler_depth > 0; }

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string shorten(const std::string& s, std::size_t n) {
    if (s.size() <= n) return s;
    return s.substr(0, n) + "…";
}

const char* error_name(httplib::Error e) {
    switch (e) {
        case httplib::Error::Success: return "success";
        case httplib::Error::Unknown: return "unknown";
        case httplib::Error::Connection: return "connection";
        case httplib::Error::BindIPAddress: return "bind";
        case httplib::Error::Read: return "read";
        case httplib::Error::Write: return "write";
        case httplib::Error::ExceedRedirectCount: return "redirect";
        case httplib::Error::Canceled: return "canceled";
        case httplib::Error::SSLConnection: return "ssl-connection";
        case httplib::Error::SSLLoadingCerts: return "ssl-certs";
        case httplib::Error::SSLServerVerification: return "ssl-verification";
        case httplib::Error::SSLServerHostnameVerification: return "ssl-hostname";
        case httplib::Error::UnsupportedMultipartBoundaryChars: return "multipart";
        case httplib::Error::Compression: return "compression";
        case httplib::Error::ConnectionTimeout: return "connect-timeout";
        case httplib::Error::ProxyConnection: return "proxy";
        case httplib::Error::ConnectionClosed: return "connection-closed";
        case httplib::Error::Timeout: return "timeout";
        case httplib::Error::ResourceExhaustion: return "resource";
        case httplib::Error::TooManyFormDataFiles: return "form-files";
        case httplib::Error::ExceedMaxPayloadSize: return "payload-too-large";
        case httplib::Error::ExceedUriMaxLength: return "uri-too-long";
        case httplib::Error::ExceedMaxSocketDescriptorCount: return "too-many-fds";
        case httplib::Error::InvalidRequestLine: return "bad-request-line";
        case httplib::Error::InvalidHTTPMethod: return "bad-method";
        case httplib::Error::InvalidHTTPVersion: return "bad-version";
        case httplib::Error::InvalidHeaders: return "bad-headers";
        case httplib::Error::MultipartParsing: return "multipart-parse";
        case httplib::Error::OpenFile: return "open-file";
        case httplib::Error::Listen: return "listen";
        default: return "other";
    }
}

Status map_error(httplib::Error e, const std::string& what) {
    const std::string detail = std::string(error_name(e)) + " (" +
                               std::to_string(static_cast<int>(e)) + ")";
    switch (e) {
        case httplib::Error::ConnectionTimeout:
        case httplib::Error::Timeout:
        case httplib::Error::Read:
            return Status(Errc::Timeout, what + " 超时: " + detail);
        default:
            return Status(Errc::IoError, what + " 失败: " + detail);
    }
}

Response to_response(const httplib::Response& r) {
    Response out;
    out.status = r.status;
    out.body   = r.body;
    for (const auto& kv : r.headers) {
        out.headers.emplace_back(kv.first, kv.second);
    }
    return out;
}

}  // namespace

// =============================================================================
//  Request / Response
// =============================================================================
std::string Request::header(std::string_view name) const {
    const std::string want = to_lower(name);
    for (const auto& kv : headers) {
        if (to_lower(kv.first) == want) return kv.second;
    }
    return {};
}

std::string Request::query_param(std::string_view name) const {
    for (const auto& kv : query) {
        if (kv.first == name) return kv.second;
    }
    return {};
}

std::string Request::param(std::string_view name) const {
    for (const auto& kv : params) {
        if (kv.first == name) return kv.second;
    }
    return {};
}

Result<my::json::Document> Request::json() const {
    if (body.empty()) return Status(Errc::InvalidArgument, "请求体为空，不是 JSON");
    return my::json::Document::parse(body);
}

void Response::set_header(const std::string& name, const std::string& value) {
    for (auto& kv : headers) {
        if (to_lower(kv.first) == to_lower(name)) {
            kv.second = value;
            return;
        }
    }
    headers.emplace_back(name, value);
}

void Response::set_content(std::string content, const std::string& content_type) {
    body = std::move(content);
    set_header("Content-Type", content_type);
}

void Response::set_json(const my::json::Document& doc) {
    set_content(doc.dump(), "application/json; charset=utf-8");
}

void Response::set_error(int code, std::string message) {
    status = code;
    my::json::Document doc;
    // 路径是字面量，不可能失败；显式 (void) 表明这是有意的忽略
    // （Status 现在是 [[nodiscard]]，不写 (void) 会告警）
    (void)doc.set("error", message);
    (void)doc.set("status", code);
    set_json(doc);
}

// =============================================================================
//  Client
// =============================================================================
struct Client::Impl {
    std::unique_ptr<httplib::Client> cli;
};

Client::Client() = default;
Client::~Client() = default;
Client::Client(Client&&) noexcept            = default;
Client& Client::operator=(Client&&) noexcept = default;

Result<Client> Client::create(const ClientOptions& options) {
    if (options.base_url.empty()) return Status(Errc::InvalidArgument, "base_url 为空");

    std::string url = options.base_url;
    if (url.rfind("http://", 0) != 0) {
        if (url.rfind("https://", 0) == 0) {
            return Status(Errc::Unsupported,
                          "本构建不支持 https（需要 CPPHTTPLIB_OPENSSL_SUPPORT + OpenSSL）");
        }
        return Status(Errc::InvalidArgument, "base_url 必须以 http:// 开头: " + options.base_url);
    }
    url = url.substr(7);
    while (!url.empty() && url.back() == '/') url.pop_back();

    // ---- host[:port] 解析 ----
    // IPv6 字面量形如 [::1]:8080，冒号有好几个：必须先定位 ']'、只在 ']' 之后找端口
    // 分隔的 ':'。原来的写法是"只要出现过 ']' 就整块跳过端口解析"，于是
    // "http://[::1]:8080" 会被当成主机名 "[::1]:8080" + 端口 80 —— 静默连到错误地址。
    std::string host;
    int         port      = 80;
    std::string port_text;
    bool        has_port  = false;

    if (!url.empty() && url.front() == '[') {
        const auto close = url.find(']');
        if (close == std::string::npos) {
            return Status(Errc::InvalidArgument,
                          "base_url 的 IPv6 地址缺少 ']': " + options.base_url);
        }
        host            = url.substr(1, close - 1);   // 去掉方括号再交给 getaddrinfo
        const auto rest = url.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') {
                return Status(Errc::InvalidArgument,
                              "base_url 的 IPv6 地址后面只能接 :port: " + options.base_url);
            }
            has_port  = true;
            port_text = rest.substr(1);
        }
    } else {
        const auto colon = url.rfind(':');
        host             = (colon == std::string::npos) ? url : url.substr(0, colon);
        if (colon != std::string::npos) {
            has_port  = true;
            port_text = url.substr(colon + 1);
        }
    }

    if (has_port) {
        // 注意不能只靠 std::stoi：它会把 "8080abc" 当成 8080 收下，
        // 所以用 consumed 校验整段都是数字。
        std::size_t consumed = 0;
        if (port_text.empty()) {
            return Status(Errc::ParseError, "base_url 的端口为空: " + options.base_url);
        }
        try {
            port = std::stoi(port_text, &consumed);
        } catch (...) {
            return Status(Errc::OutOfRange, "端口不是合法数字: " + port_text);
        }
        if (consumed != port_text.size()) {
            return Status(Errc::ParseError, "端口含非数字字符: " + port_text);
        }
        if (port <= 0 || port > 65535) return Status(Errc::OutOfRange, "端口越界: " + port_text);
    }
    if (host.empty()) return Status(Errc::InvalidArgument, "base_url 缺少主机名");
    if (host.find('/') != std::string::npos) {
        return Status(Errc::InvalidArgument,
                      "base_url 只能是 http://host[:port]，不能带路径: " + options.base_url);
    }

    Client c;
    c.impl_ = std::make_unique<Impl>();
    try {
        c.impl_->cli = std::make_unique<httplib::Client>(host, port);
    } catch (const std::exception& e) {
        return Status(Errc::RuntimeError, std::string("创建 HTTP 客户端失败: ") + e.what());
    }
    c.impl_->cli->set_connection_timeout(std::chrono::milliseconds(options.connect_timeout_ms));
    c.impl_->cli->set_read_timeout(std::chrono::milliseconds(options.read_timeout_ms));
    c.impl_->cli->set_write_timeout(std::chrono::milliseconds(options.read_timeout_ms));
    c.impl_->cli->set_default_headers({{"User-Agent", options.user_agent}});
    return c;
}

bool Client::valid() const noexcept { return impl_ != nullptr && impl_->cli != nullptr; }

Result<Response> Client::request(const std::string& method, const std::string& path,
                                 const std::string& body, const std::string& content_type,
                                 const Headers& extra_headers) {
    if (!valid()) return Status(Errc::NotInitialized, "Client 未创建");
    if (path.empty()) return Status(Errc::InvalidArgument, "path 为空");

    httplib::Request req;
    req.method = method;
    req.path   = path;
    req.body   = body;
    if (!content_type.empty()) req.set_header("Content-Type", content_type);
    for (const auto& kv : extra_headers) req.set_header(kv.first, kv.second);

    try {
        auto res = impl_->cli->send(req);
        if (!res) return map_error(res.error(), method + " " + path);
        return to_response(res.value());
    } catch (const std::exception& e) {
        return Status(Errc::RuntimeError, std::string("HTTP 请求异常: ") + e.what());
    }
}

Result<Response> Client::get(const std::string& path) {
    return request("GET", path);
}

Result<Response> Client::post(const std::string& path, const std::string& body,
                              const std::string& content_type) {
    return request("POST", path, body, content_type);
}

Result<Response> Client::put(const std::string& path, const std::string& body,
                             const std::string& content_type) {
    return request("PUT", path, body, content_type);
}

Result<Response> Client::del(const std::string& path) {
    return request("DELETE", path);
}

Result<my::json::Document> Client::get_json(const std::string& path) {
    auto r = get(path);
    if (!r) return r.status();
    if (r.value().status < 200 || r.value().status >= 300) {
        return Status(Errc::RuntimeError,
                      "HTTP " + std::to_string(r.value().status) + " 于 " + path + ": " +
                          shorten(r.value().body, 200));
    }
    return my::json::Document::parse(r.value().body);
}

Result<Response> Client::post_json(const std::string& path, const my::json::Document& doc) {
    return post(path, doc.dump(), "application/json");
}

// =============================================================================
//  Server
// =============================================================================
struct Server::Impl {
    httplib::Server         svr;
    ServerOptions           opts;
    std::thread             thread;
    std::atomic<bool>       running{false};
    std::atomic<bool>       stopped{false};   ///< stop() 主动停止 vs 真正的监听失败
    std::uint16_t           bound_port = 0;
    /// 保护 thread 的 joinable()/join()/detach()：并发 stop() 会让两个线程
    /// 同时对同一个 joinable thread 调 join()（未定义行为）
    std::mutex              thread_mutex;
};

Server::Server() = default;
Server::~Server() { stop(); }
Server::Server(Server&&) noexcept = default;

// 默认移动赋值会先析构目标对象的 Impl，而 Impl 里的 thread 可能还 joinable
// （listen_async 之后）—— 那会调 std::terminate。所以先 stop() 再接管。
Server& Server::operator=(Server&& other) noexcept {
    if (this != &other) {
        stop();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

Result<Server> Server::create(const ServerOptions& options) {
    Server s;
    s.impl_      = std::make_unique<Impl>();
    s.impl_->opts = options;

    auto& svr = s.impl_->svr;
    svr.set_read_timeout(options.read_timeout_s, 0);
    svr.set_write_timeout(options.write_timeout_s, 0);
    svr.set_keep_alive_max_count(8);
    svr.set_payload_max_length(static_cast<size_t>(options.max_payload_bytes));

    int port = 0;
    if (options.port == 0) {
        port = svr.bind_to_any_port(options.host);
    } else if (svr.bind_to_port(options.host, options.port)) {
        port = static_cast<int>(options.port);
    }
    if (port <= 0) {
        return Status(Errc::IoError,
                      "HTTP 端口绑定失败: " + options.host + ":" + std::to_string(options.port) +
                          "（端口被占用？）");
    }
    s.impl_->bound_port = static_cast<std::uint16_t>(port);
    return s;
}

Status Server::route(const std::string& method, const std::string& path, Handler handler) {
    if (!impl_) return Status(Errc::NotInitialized, "Server 未创建");
    if (!handler) return Status(Errc::InvalidArgument, "handler 为空");
    if (path.empty()) return Status(Errc::InvalidArgument, "路由 path 为空");
    auto fn = [h = std::move(handler)](const httplib::Request& req, httplib::Response& res) {
        Request r;
        r.method = req.method;
        r.path   = req.path;
        r.body   = req.body;
        for (const auto& kv : req.headers) r.headers.emplace_back(kv.first, kv.second);
        // httplib 的语义：req.params 是**已解码的 query 参数**，
        // req.path_params 是路由 /x/:id 捕获的**路径参数**。
        for (const auto& kv : req.params) r.query.emplace_back(kv.first, kv.second);
        for (const auto& kv : req.path_params) r.params.emplace_back(kv.first, kv.second);

        Response out;
        {
            // 标记"当前线程正在执行 handler"：stop() 需要据此判断能不能 join。
            // httplib 的 handler 跑在**线程池的 worker** 上，不是监听线程，
            // 所以"线程 id == 监听线程 id"这种判断是不够的。
            HandlerScope scope;
            try {
                h(r, out);
            } catch (const std::exception& e) {
                out.set_error(500, std::string("handler 抛异常: ") + e.what());
            } catch (...) {
                out.set_error(500, "handler 抛未知异常");
            }
        }

        res.status = out.status;
        res.body   = out.body;
        for (const auto& kv : out.headers) res.set_header(kv.first, kv.second);
        // 没设 Content-Type 时给个默认值，避免客户端拿到 application/octet-stream
        const bool has_ct =
            std::any_of(out.headers.begin(), out.headers.end(),
                        [](const auto& kv) { return to_lower(kv.first) == "content-type"; });
        if (!out.body.empty() && !has_ct) {
            res.set_header("Content-Type", "text/plain; charset=utf-8");
        }
    };

    const std::string m = to_lower(method);
    if (m == "get") {
        impl_->svr.Get(path, fn);
    } else if (m == "post") {
        impl_->svr.Post(path, fn);
    } else if (m == "put") {
        impl_->svr.Put(path, fn);
    } else if (m == "patch") {
        impl_->svr.Patch(path, fn);
    } else if (m == "delete") {
        impl_->svr.Delete(path, fn);
    } else if (m == "options") {
        impl_->svr.Options(path, fn);
    } else {
        return Status(Errc::Unsupported,
                      "不支持的 HTTP 方法: " + method + "（可用 GET/POST/PUT/PATCH/DELETE/OPTIONS）");
    }
    return Ok();
}

/// 便捷重载（void 版）失败时的上报出口。
///
/// 这六个 void 重载唯一可能的失败是"Server 未创建"（方法名是字面量，不会走到
/// Unsupported 分支）。以前用 `(void)route(...)` 静默丢掉，于是
/// `Server s; s.get("/x", h);` 这种用法看起来注册成功、实际什么都没发生。
/// 现在至少往 stderr 留一行，配合 Status 的 [[nodiscard]] 能早发现。
void report_route_failure(const Status& st, const char* method, const std::string& path) {
    std::fprintf(stderr, "[my_cpp_libs] Server::%s(\"%s\") 路由注册失败: %s\n", method,
                 path.c_str(), st.to_string().c_str());
}

void Server::get(const std::string& path, Handler handler) {
    if (auto st = route("GET", path, std::move(handler)); !st) {
        report_route_failure(st, "get", path);
    }
}
void Server::post(const std::string& path, Handler handler) {
    if (auto st = route("POST", path, std::move(handler)); !st) {
        report_route_failure(st, "post", path);
    }
}
void Server::put(const std::string& path, Handler handler) {
    if (auto st = route("PUT", path, std::move(handler)); !st) {
        report_route_failure(st, "put", path);
    }
}
void Server::patch(const std::string& path, Handler handler) {
    if (auto st = route("PATCH", path, std::move(handler)); !st) {
        report_route_failure(st, "patch", path);
    }
}
void Server::del(const std::string& path, Handler handler) {
    if (auto st = route("DELETE", path, std::move(handler)); !st) {
        report_route_failure(st, "del", path);
    }
}
void Server::options(const std::string& path, Handler handler) {
    if (auto st = route("OPTIONS", path, std::move(handler)); !st) {
        report_route_failure(st, "options", path);
    }
}

Status Server::set_mount_point(const std::string& mount_point, const std::string& dir) {
    if (!impl_) return Status(Errc::NotInitialized, "Server 未创建");
    if (!impl_->svr.set_mount_point(mount_point, dir)) {
        return Status(Errc::IoError, "挂载静态目录失败: " + dir);
    }
    return Ok();
}

Status Server::listen() {
    if (!impl_) return Status(Errc::NotInitialized, "Server 未创建");
    impl_->stopped = false;
    impl_->running = true;
    const bool ok  = impl_->svr.listen_after_bind();
    impl_->running = false;
    // stop() 主动停止时 httplib 也会返回 false（监听 fd 已被关掉），这不算故障
    if (!ok && !impl_->stopped.load()) return Status(Errc::IoError, "HTTP 服务异常退出");
    return Ok();
}

Status Server::listen_async() {
    if (!impl_) return Status(Errc::NotInitialized, "Server 未创建");

    // 已经跑起来了直接返回；用 CAS 避免两个线程同时进
    bool expected = false;
    if (!impl_->running.compare_exchange_strong(expected, true)) return Ok();

    // 上一次的后台线程如果已经自然结束，先回收，否则对 joinable thread 赋值 = terminate
    {
        // 回收旧线程 + 赋新线程必须在**同一把锁**里：stop() 会在另一个线程读
        // joinable()/get_id()，锁外赋值就是实打实的数据竞争（TSan 能抓到）。
        std::lock_guard<std::mutex> lock(impl_->thread_mutex);
        if (impl_->thread.joinable()) impl_->thread.join();  // 该线程已结束，join 立即返回
        impl_->stopped = false;
        impl_->thread  = std::thread([impl = impl_.get()] {
            impl->svr.listen_after_bind();
            impl->running = false;
        });
    }

    // 等后台线程真正进入 accept 循环（或立刻失败），避免把"启动失败"报成成功
    for (int i = 0; i < 200; ++i) {
        if (impl_->svr.is_running()) return Ok();
        if (!impl_->running.load()) {
            if (impl_->thread.joinable()) impl_->thread.join();
            return Status(Errc::IoError, "HTTP 监听启动失败（端口已被占用或已被 stop）");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return Ok();  // 还没就绪但也没报错：交给调用方用 running() 判断
}

void Server::stop() {
    if (!impl_) return;
    impl_->stopped = true;
    impl_->svr.stop();

    // 关键判定：handler 上下文（线程池 worker）里绝不能 join 监听线程，
    // 否则就是"worker 等监听线程、监听线程 join 所有 worker"的循环等待（实测死锁）。
    const bool from_handler = in_handler_context();
    bool       self_thread  = false;
    {
        // 读 joinable()/get_id() 同样要拿锁：listen_async 可能在另一个线程赋值。
        std::lock_guard<std::mutex> lock(impl_->thread_mutex);
        self_thread = impl_->thread.joinable() &&
                      impl_->thread.get_id() == std::this_thread::get_id();
    }
    if (from_handler || self_thread) {
        // detach 路径连互斥锁都用 try_lock：如果另一个线程正拿着锁 join 收尾，
        // 我们在这里**阻塞**就会再次形成同样的环（它等监听线程，监听线程等我们）。
        // 拿不到锁就直接返回 —— 那边的 join 会把线程收干净。
        std::unique_lock<std::mutex> lock(impl_->thread_mutex, std::try_to_lock);
        if (lock.owns_lock() && impl_->thread.joinable()) {
            // detach 之后 running 由监听线程在退出时自己置 false，
            // 于是外部"等到 running()==false"就等价于"监听线程真的退出了"，
            // 此时才允许析构 Server（detach 路径下唯一的生命周期约定）。
            impl_->thread.detach();
        }
        return;
    }

    std::lock_guard<std::mutex> lock(impl_->thread_mutex);
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->running = false;
}

std::uint16_t Server::port() const {
    return impl_ ? impl_->bound_port : 0;
}

bool Server::running() const {
    return impl_ && impl_->running.load();
}

}  // namespace my::http
