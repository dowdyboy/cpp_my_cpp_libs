# 网络与 HTTP（TCP / UDP / HTTP 客户端与服务端）

> 结论：**TCP/UDP 不引库（自己封），HTTP 引 cpp-httplib（单头文件，MIT）**。
> 两个组件都验证过 MinGW / MSVC / armv7（PetaLinux SDK）编译；
> 板端 HTTP 服务端可以直接给你上位机的 Flask 打数据，也可以反过来给板子配一个 REST 接口。

---

## 1. 组件划分

```
my::net   —— TCP 客户端/服务端、UDP（广播/组播）、超时、分帧   （无三方依赖）
my::http  —— HTTP 客户端 + 服务端（封装 cpp-httplib）           （依赖 httplib + json）
```

`http` 依赖 `net` 吗？不依赖：httplib 自己带 socket 实现。两者是并列的，
`net` 用在"自定义二进制/TLV 协议、遥测、发现"，`http` 用在"REST/JSON"。

## 2. 为什么 TCP/UDP 不引库

需要处理的平台差异就这么几条，集中在 `src/net/net.cpp` 一个文件里：

| 差异 | 处理 |
|---|---|
| Windows 要 `WSAStartup/WSACleanup` | 进程级单例，`ensure_wsa()` |
| `SOCKET` vs `int`、`closesocket` vs `close` | 内部统一存 `std::intptr_t`，`close_fd()` 收口 |
| `errno` vs `WSAGetLastError` | `MYLIB_SOCK_ERRNO()` + `errno_text()`，Status 里带错误码 |
| **SIGPIPE**：Linux 写已关闭连接会**杀进程** | POSIX 侧 `send(..., MSG_NOSIGNAL)`；有专门用例守 |
| 非阻塞 | Windows `ioctlsocket(FIONBIO)` / POSIX `fcntl(O_NONBLOCK)` |
| 收发超时 | Windows `DWORD` 毫秒 / POSIX `timeval` |
| 连接超时 | `connect()` 非阻塞 + `select` + `SO_ERROR` |
| 地址解析 | `getaddrinfo`（AF_UNSPEC，优先返回 IPv4），`inet_ntop` 反向 |
| IPv6 | `parse_endpoint` 支持 `[::1]:port` 写法 |

引 Asio/libuv 的收益在"事件驱动 + 高并发"，本项目目前是"一条控制链路 + 一路遥测"，
用不上；真要上（>50 并发、需要 epoll 事件循环）再换，`my::net` 的接口可以保持不变。

## 3. 为什么 HTTP 引 cpp-httplib

| 项 | 实测结果 |
|---|---|
| 版本 | v0.66.0（commit `cdcd2ac6…`），**MIT** |
| 形态 | 单头 `httplib.h`（862 KB），同时有 `Client` 和 `Server` |
| MinGW 编译 | 通过；单 TU 约 **17 s**；spike 可执行文件 1.29 MB（未 strip） |
| armv7 交叉编译 | 通过（PetaLinux 2020.2 / GCC 9.2）：ELF32 ARM EABI5 HF |
| 板端体积 | **strip 后 291 KB**；`-fno-exceptions` 版 250 KB |
| 依赖 | 只要 `libpthread/libstdc++/libm/libgcc_s/libc`，不拉 OpenSSL/zlib |
| Windows 坑 | 必须 `_WIN32_WINNT=0x0A00`（否则直接 `#error`），链接 `ws2_32` |

不选其它库的原因见 [01 §2.1](01-规划与可行性.md) 的网络一节：libcurl 只做客户端且要 TLS
依赖；civetweb 只有服务端且是 C API；mongoose 是 GPL/商业；libmicrohttpd 是 LGPL。

## 4. `my::net` 用法

```cpp
#include <my_cpp_libs/net/net.hpp>

// ---- TCP 服务端 ----
auto listener = my::net::TcpListener::bind({"0.0.0.0", 9000}, /*backlog=*/8);
if (!listener) { my::log::error("{}", listener.status().to_string()); return; }

auto conn = listener.value().accept(/*timeout_ms=*/1000);   // 超时返回 Errc::Timeout
if (conn) {
    std::uint8_t buf[4096];
    auto n = conn.value().recv(buf, sizeof(buf));            // n==0 表示对端关闭
    if (n && n.value() > 0) {
        conn.value().send_all("ok", 2);                      // 内部处理部分写
    }
}

// ---- TCP 客户端 ----
auto cli = my::net::TcpSocket::connect({"192.168.1.50", 9000}, 2000);  // 连接超时
cli.value().set_timeout(1000);                                         // 收发超时

// ---- 分帧（板端 <-> 上位机自定义协议）----
my::net::write_frame(cli.value(), R"({"cmd":"start"})");
auto payload = my::net::read_frame(cli.value(), 64 * 1024);   // 4 字节大端长度 + 负载

// ---- UDP ----
auto rx = my::net::UdpSocket::bind({"0.0.0.0", 9001});
auto tx = my::net::UdpSocket::open();
tx.value().set_broadcast(true);
tx.value().send_to({"255.255.255.255", 9001}, data, size);
rx.value().join_multicast("239.255.0.1");                     // 组播发现
auto d = rx.value().recv_from();                              // {data, from}
```

约定：

- 失败全部是 `Status`，消息里带 errno/WSA 码；
- `recv()` 返回 **0 = 对端正常关闭**（POSIX 语义），出错才返回 Status；
- `send()` 可能部分写，`send_all()` 才保证写完；
- 单个 socket 不做内部加锁：允许"一读一写"两线程，不要多线程同时 `recv`。

## 5. `my::http` 用法

```cpp
#include <my_cpp_libs/http/http.hpp>

// ---- 客户端：把数据打到上位机的 Flask ----
my::http::ClientOptions opt;
opt.base_url          = "http://192.168.1.50:5000";
opt.connect_timeout_ms = 2000;
opt.read_timeout_ms    = 5000;

auto cli = my::http::Client::create(opt);
my::json::Document doc;
doc.set("device", "zynq-7020");
doc.set("temp_c", 36.5);
auto r = cli.value().post_json("/api/upload", doc);   // 自动 Content-Type: application/json
if (!r) my::log::error("上报失败: {}", r.status().to_string());

// 带自定义 header（鉴权/设备号）
my::http::Headers h{{"X-Device-Id", "zynq-7020-01"}};
cli.value().request("GET", "/api/status", "", "", h);

// ---- 服务端：板端 REST ----
my::http::ServerOptions sopt;
sopt.host = "0.0.0.0";
sopt.port = 8080;
auto srv = my::http::Server::create(sopt);
srv.value().get("/status", [](const my::http::Request&, my::http::Response& res) {
    my::json::Document d; d.set("ok", true); d.set("uptime_s", 1234);
    res.set_json(d);
});
srv.value().post("/config", [](const my::http::Request& req, my::http::Response& res) {
    auto doc = req.json();
    if (!doc) { res.set_error(400, "请求体不是 JSON"); return; }
    // ... 应用配置 ...
    res.set_json(doc.value());
});
srv.value().listen();   // 阻塞；stop() 从别的线程停
```

设计约定：

- **4xx/5xx 不是 Status 失败**：网络层成功就返回 `Response`，`status` 自己看；
  `get_json()` 例外（非 2xx 转 `RuntimeError`，消息里带状态码和 body 前 200 字节）；
- `set_error(code, msg)` 会生成 `{"error": "...", "status": code}`，
  上位机/Flask 侧可以直接按 JSON 解析；
- handler 抛异常会被捕获成 500，不会把服务端带崩；
- 公共头里没有任何 httplib 类型（pimpl 隔离）；
- `Client` 不是线程安全的（一个线程一个 Client）；`Server` 内部是线程池。

## 6. 板端注意事项

1. **线程池要调小**：`-DMYLIB_HTTP_THREAD_POOL=2`（默认 4）。httplib 的默认值是
   `max(8, 核数-1)`，在 A9 双核上等于白占内存。
2. **端口**：PetaLinux rootfs 里 80/8080 一般不冲突；如果装了 nginx/Apache 先查一下。
3. **防火墙**：Windows 上第一次起服务端会弹防火墙确认；板端没有这一层。
4. **超时**：`read_timeout_s` 默认 5 s，慢 handler（比如等 ADC 采集）要相应调大。
5. **keep-alive**：服务端默认最多 8 个 keep-alive 连接，客户端 httplib 默认复用连接；
   频繁短连接（每帧一个请求）建议走 keep-alive，省掉三次握手。
6. **TLS 未启用**：只支持 `http://`；`https://` 会明确返回 `Unsupported`。
   需要 HTTPS 时给 httplib 定义 `CPPHTTPLIB_OPENSSL_SUPPORT` 并链接 OpenSSL/mbedTLS，
   建议只在"板子接外网"时开，局域网内用 HTTP + 自己的鉴权头。

## 7. 测试覆盖（本机回环，不依赖外部网络，板端同样全过）

| 套件 | 用例数 | 覆盖 |
|---|---|---|
| `net` | 15 | 地址解析、TCP 回显、accept/connect 超时、对端关闭、**SIGPIPE 不杀进程**、分帧（含超大长度拒绝）、行协议、UDP 往返/超时/广播/组播参数校验 |
| `http` | 11 | GET/POST、JSON 往返、query/路径参数、未知方法拒绝、自定义 header、404 与错误体、读超时、连接拒绝、stop 释放端口、非法 base_url、未初始化客户端 |

```bash
ctest --preset win-mingw -R "net_|http_" --output-on-failure
```

## 8. 还没做 / 后续

| 需求 | 方案 |
|---|---|
| HTTPS | httplib + OpenSSL（或 mbedTLS），做成 `MYLIB_ENABLE_HTTP_TLS` 开关 |
| WebSocket | httplib 自带 `WebSocket` 支持（同一头文件），需要时加 `my::http::WsServer/Client` |
| 高并发/事件驱动 | 换 Asio standalone（接口不变，`my::net` 内部替换） |
| 代理/重定向 | httplib 有 `set_proxy`/`set_follow_location`，按需暴露 |
| ~~板端实测~~ | ✅ **已完成（2026-11-16）**：`net` 15/15、`http` 11/11 通过；`example_net` 36 ms、`example_http` 141 ms |
