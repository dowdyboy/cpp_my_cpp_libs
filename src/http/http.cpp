#include "my_cpp_libs/http/http.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include "httplib.h"

namespace my::http {
namespace {

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
    doc.set("error", message);
    doc.set("status", code);
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

    std::string host = url;
    int         port = 80;
    const auto  colon = url.rfind(':');
    if (colon != std::string::npos && url.find(']') == std::string::npos) {
        host = url.substr(0, colon);
        try {
            port = std::stoi(url.substr(colon + 1));
        } catch (...) {
            return Status(Errc::ParseError, "端口不是数字: " + url.substr(colon + 1));
        }
        if (port <= 0 || port > 65535) return Status(Errc::OutOfRange, "端口越界");
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
        try {
            h(r, out);
        } catch (const std::exception& e) {
            out.set_error(500, std::string("handler 抛异常: ") + e.what());
        } catch (...) {
            out.set_error(500, "handler 抛未知异常");
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

void Server::get(const std::string& path, Handler handler) {
    (void)route("GET", path, std::move(handler));
}
void Server::post(const std::string& path, Handler handler) {
    (void)route("POST", path, std::move(handler));
}
void Server::put(const std::string& path, Handler handler) {
    (void)route("PUT", path, std::move(handler));
}
void Server::patch(const std::string& path, Handler handler) {
    (void)route("PATCH", path, std::move(handler));
}
void Server::del(const std::string& path, Handler handler) {
    (void)route("DELETE", path, std::move(handler));
}
void Server::options(const std::string& path, Handler handler) {
    (void)route("OPTIONS", path, std::move(handler));
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
    if (impl_->thread.joinable()) impl_->thread.join();

    impl_->stopped = false;
    impl_->thread  = std::thread([impl = impl_.get()] {
        impl->svr.listen_after_bind();
        impl->running = false;
    });

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
