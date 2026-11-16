#pragma once
// =============================================================================
//  my::http —— HTTP 客户端 + 服务端（封装 cpp-httplib）
//
//  为什么引 cpp-httplib（而不是自己写 HTTP）：
//    * 一个 MIT 单头文件同时覆盖 client 和 server，不需要 OpenSSL/zlib 也能编；
//    * HTTP/1.1 的细节（keep-alive、分块、multipart、超时、header 大小写）自己做
//      要几百行且容易踩坑 —— 这是"写出来一定错"的那一类；
//    * 已验证：MinGW / MSVC / armv7（PetaLinux SDK）都能编，strip 后约 290 KB。
//
//  与项目其余组件一致的约定：
//    * 公共头**不出现** httplib 的任何类型（pimpl 隔离，和 infer 隔离 Ort::* 一样）；
//    * 失败一律 `Status`；HTTP 状态码在 `Response::status` 里，不当错误；
//    * 4xx/5xx 不算 `Status` 失败 —— 网络层成功就返回 Response，业务自己判断。
//
//  线程：Server 内部是线程池（编译期 CPPHTTPLIB_THREAD_POOL_COUNT，
//  CMake 变量 MYLIB_HTTP_THREAD_POOL 可配；板端建议 2）。
//  Client 不是线程安全的：一个线程一个 Client。
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "my_cpp_libs/core/status.hpp"
#include "my_cpp_libs/export.hpp"
#include "my_cpp_libs/json/json.hpp"

namespace my::http {

using Headers = std::vector<std::pair<std::string, std::string>>;

// =============================================================================
//  请求 / 响应
// =============================================================================
struct Request {
    std::string method;
    std::string path;    ///< 不含 query，如 "/api/item/42"
    std::string body;
    Headers     headers;
    /// URL 查询参数：/x?a=1&b=2 → {{"a","1"},{"b","2"}}
    std::vector<std::pair<std::string, std::string>> query;
    /// 路径参数：路由写 /api/item/:id → {{"id","42"}}
    std::vector<std::pair<std::string, std::string>> params;

    /// 取 header（名字大小写不敏感）
    MYLIB_API std::string header(std::string_view name) const;
    /// 取 query 参数；没有则返回空串
    MYLIB_API std::string query_param(std::string_view name) const;
    /// 取路径参数；没有则返回空串
    MYLIB_API std::string param(std::string_view name) const;

    /// 把 body 当 JSON 解析
    MYLIB_API Result<my::json::Document> json() const;
};

struct Response {
    int         status = 200;
    Headers     headers;
    std::string body;

    MYLIB_API void set_header(const std::string& name, const std::string& value);
    MYLIB_API void set_content(std::string content,
                               const std::string& content_type = "text/plain; charset=utf-8");
    MYLIB_API void set_json(const my::json::Document& doc);

    /// 统一的错误体：{"error": "..."}，方便上位机/Flask 侧直接解析
    MYLIB_API void set_error(int code, std::string message);
};

// =============================================================================
//  客户端
// =============================================================================
struct ClientOptions {
    /// 形如 "http://192.168.1.50:5000"（只支持 http://，TLS 见文档）
    std::string base_url;
    int         connect_timeout_ms = 2000;
    int         read_timeout_ms    = 5000;
    std::string user_agent         = "my_cpp_libs-http/0.1";
};

class Client {
public:
    Client();
    ~Client();
    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;
    Client(const Client&)            = delete;
    Client& operator=(const Client&) = delete;

    MYLIB_API static Result<Client> create(const ClientOptions& options);

    /// 通用请求。path 可以带 query，如 "/api/x?a=1"
    MYLIB_API Result<Response> request(const std::string& method, const std::string& path,
                                       const std::string& body = std::string{},
                                       const std::string& content_type = std::string{},
                                       const Headers&     extra_headers = {});

    MYLIB_API Result<Response> get(const std::string& path);
    MYLIB_API Result<Response> post(const std::string& path, const std::string& body,
                                    const std::string& content_type = "application/json");
    MYLIB_API Result<Response> put(const std::string& path, const std::string& body,
                                   const std::string& content_type = "application/json");
    MYLIB_API Result<Response> del(const std::string& path);

    /// 2xx 才返回 Document；非 2xx → RuntimeError（消息里带状态码和 body 前 200 字节）
    MYLIB_API Result<my::json::Document> get_json(const std::string& path);
    MYLIB_API Result<Response> post_json(const std::string& path,
                                         const my::json::Document& doc);

    MYLIB_API bool valid() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// =============================================================================
//  服务端
// =============================================================================
struct ServerOptions {
    std::string host              = "0.0.0.0";
    std::uint16_t port            = 8080;  ///< 0 = 让系统分配（用 port() 取）
    int         read_timeout_s    = 5;
    int         write_timeout_s   = 5;
    std::size_t max_payload_bytes = 8 * 1024 * 1024;
};

class Server {
public:
    using Handler = std::function<void(const Request&, Response&)>;

    Server();
    ~Server();
    Server(Server&&) noexcept;
    Server& operator=(Server&&) noexcept;
    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    /// 创建并**绑定**端口（端口被占用在这里就能发现）
    MYLIB_API static Result<Server> create(const ServerOptions& options);

    /// 注册路由。支持 GET/POST/PUT/PATCH/DELETE/OPTIONS；
    /// 其它方法返回 Errc::Unsupported（不再静默忽略）。
    MYLIB_API Status route(const std::string& method, const std::string& path, Handler handler);
    MYLIB_API void   get(const std::string& path, Handler handler);
    MYLIB_API void   post(const std::string& path, Handler handler);
    MYLIB_API void   put(const std::string& path, Handler handler);
    MYLIB_API void   patch(const std::string& path, Handler handler);
    MYLIB_API void   del(const std::string& path, Handler handler);
    MYLIB_API void   options(const std::string& path, Handler handler);

    /// 静态文件目录（可选）
    MYLIB_API Status set_mount_point(const std::string& mount_point, const std::string& dir);

    /// 阻塞直到 stop()
    MYLIB_API Status listen();
    /// 后台线程启动（测试/业务里非阻塞启动）
    MYLIB_API Status listen_async();
    /// 停止并等待线程结束
    MYLIB_API void stop();

    MYLIB_API std::uint16_t port() const;
    MYLIB_API bool          running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace my::http
