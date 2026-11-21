#pragma once
// =============================================================================
//  my::net::TcpServer —— accept 循环 + 连接上限 + 空闲超时 + 每连接处理
//
//  由来：板端每个 TCP 服务都在手写同一段骨架 ——
//      listener.accept() → 判连接数 → 起线程 → set_timeout → 收循环 → 关连接
//  这段代码每抄一次就多一份"忘了关 socket / 上限没跟着线程数同步 / stop() 停不
//  下来"的风险。本类把这套骨架收进库里，业务只写 handler。
//
//  --------------------------------------------------------------- 线程模型
//    * 1 个 accept 线程：循环 `listener.accept(accept_timeout_ms)`。
//      accept_timeout_ms 同时是 stop() 的最坏延迟 —— 不用"从另一个线程 close
//      listener 去打断 accept"的写法（同一个 fd 上并发 select + close 是数据
//      竞争，Windows 上句柄号还可能被回收给别的 socket）。
//    * 每条连接 1 个 **detached** 线程：handler 在独立线程里跑，返回后连接被关闭。
//    * 线程只持有 `shared_ptr<Impl>`，不碰 TcpServer 对象本身 —— 对象先析构、
//      连接线程后退出也是安全的。
//    * stop() 有界：join accept 线程（≤ accept_timeout_ms）+ 最多等 5 s 让在途
//      连接退出；等不到就不等了（detached 线程继续跑完自己收尾）。
//
//  --------------------------------------------------------------- 适用边界
//    每连接一个线程，**不是**高并发服务器：上百连接时线程栈（默认每个线程几 MB
//    虚拟地址空间）+ 上下文切换会先撞墙，所以默认 max_connections = 8，按"一条
//    控制链路 + 少量遥测连接"的板端场景定的。真要几百上千并发，换 epoll/io_uring
//    的库（Asio/libuv）；本类只解决"别再把这段骨架抄进业务"。
//    另外：handler 在连接线程里跑，它卡住只影响那一条连接（stop() 有界等待兜底），
//    但 handler 里**不要**调用本对象的 stop()/析构 —— 那是自己等自己（有界 5 s
//    后返回，连接在 handler 返回后才关）。
//
//  --------------------------------------------------------------- 构建边界
//    完整实现需要 std::thread、std::function 和异常边界（拿异常兜住 handler 与
//    线程创建），只在**开异常**的构建里可用。`-fno-exceptions` 构建
//    （-DMYLIB_NO_EXCEPTIONS=1，见 cmake/Options.cmake）里：
//      * 类型与全部成员**照常声明**，头文件不变化 —— 消费方代码照常编译通过，
//        不会出现"编译过、运行时链接不到符号"的洞；
//      * 实现退化成桩：create()/start() 返回 Errc::Unsupported，stop() 空转，
//        running()/计数返回 false/0，运行时给出明确错误；
//      * net 的其余部分（TcpSocket/TcpListener/分帧/UDP）不受影响，仍在 noex 子集。
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "my_cpp_libs/export.hpp"
#include "my_cpp_libs/net/net.hpp"

namespace my::net {

// =============================================================================
//  配置
// =============================================================================
struct TcpServerOptions {
    /// 同时处理的连接上限。**必须 >= 1**：0 会让 create() 返回 InvalidArgument，
    /// 不做"0 = 不限制"的语义 —— 少写一个上限就退化成"每个连接起一个线程"，
    /// 这种默认值太危险，宁可在构造时直接拒绝。
    std::size_t max_connections = 8;

    /// 每连接 socket 的收发超时（毫秒）。0 = 不设（阻塞）。
    /// 注意它对 handler 是**透明**的：handler 里 recv/read_line 超时会拿到
    /// Errc::Timeout。设成 0 时 handler 可能永久阻塞，此时 stop() 只能等满 5 s
    /// 后放弃等待（线程仍安全，只是收不了尾）。
    int idle_timeout_ms = 30000;

    /// accept 轮询粒度（毫秒），决定 stop() 的最坏延迟。
    /// 0 会变成 select(0) 忙轮询，内部按 1 ms 处理；负数 → InvalidArgument。
    int accept_timeout_ms = 200;

    /// 内核 accept 队列长度；<= 0 按 1 处理。
    int backlog = 16;

    /// SO_REUSEADDR。默认 false：让端口冲突在 create() 就暴露。
    /// （Windows 上 SO_REUSEADDR 允许两个进程绑同一端口，等于把冲突检测和端口
    ///   劫持防护一起废掉，所以服务端默认不开。）
    bool reuse_addr = false;
};

// =============================================================================
//  TcpServer
// =============================================================================
class TcpServer {
public:
    /// 每个连接的 handler，在**独立的连接线程**里调用；返回后该连接被关闭。
    ///
    /// 约定：
    ///   * 不要把这个 socket 传出去给别的线程长期使用（生命周期到 handler 返回）；
    ///   * 抛出的异常会被吞掉并计入 handler_errors()，连接照常关闭；
    ///   * 不要在 handler 里调用本对象的 stop()/析构（见文件头说明）。
    using Handler = std::function<void(TcpSocket&, const Endpoint&)>;

    /// 创建并 bind（**不**启动 accept 线程）。唯一构造入口。
    ///
    /// 端口冲突、非法地址这类错误在这里就返回，不会拖到 start()。ep.port == 0
    /// 时由系统分配，用 port() 取实际端口。
    ///
    /// 失败：handler 为空 / max_connections == 0 / 超时为负 → InvalidArgument；
    ///       bind 失败（端口占用、地址不可用）→ IoError；内存不足 → RuntimeError。
    MYLIB_API static Result<TcpServer> create(const Endpoint& ep, Handler handler,
                                              const TcpServerOptions& options = {});

    /// 停掉 accept 线程并关闭 listener（幂等）。见 stop()。
    MYLIB_API ~TcpServer();

    /// 移动后源对象的 impl_ 为空：源对象上的 start()/查询安全（返回 NotInitialized
    /// 或 0/false），析构不会做任何事。移动赋值会先停掉自己原本持有的那一台。
    MYLIB_API TcpServer(TcpServer&&) noexcept;
    MYLIB_API TcpServer& operator=(TcpServer&&) noexcept;
    TcpServer(const TcpServer&)            = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    /// 启动 accept 线程。已在运行 → Busy。
    /// stop() 之后再 start() 会按 create() 时的**实际端口**重新 bind，所以同一个
    /// 对象可以反复"start → stop → start"（前提是那个端口还没被别人抢走）。
    MYLIB_API Status start();

    /// 幂等停止：置停止标志 → 关 listener → join accept 线程（≤ accept_timeout_ms）
    /// → 最多等 5 s 让在途连接退出（等不到就放弃等待，detached 连接线程继续跑完）。
    /// 这期间新来的连接不再被 accept（监听 socket 已关），已 accept 但还没起线程的
    /// 会被直接关闭。
    MYLIB_API void stop() noexcept;

    MYLIB_API bool running() const noexcept;

    /// 实际监听的端口（create 时 ep.port == 0 就用它取系统分配的端口）。
    /// stop() 之后仍返回最后绑定到的端口；对象被移动后（源对象）返回 0。
    MYLIB_API std::uint16_t port() const;

    /// 当前在途连接数（已交给 handler、线程还没退出的连接）。
    MYLIB_API std::size_t active_connections() const noexcept;

    /// 成功交给 handler 的连接总数（**不是** accept 到的数目：因上限被拒、
    /// 因停止被丢弃、线程创建失败的都不算）。重连/重复 start 不清零。
    MYLIB_API std::uint64_t accepted_total() const noexcept;

    /// 因 max_connections 被拒并直接关闭的连接数。
    MYLIB_API std::uint64_t rejected_total() const noexcept;

    /// handler 抛异常的次数（异常被吞掉，连接照常关闭）。
    MYLIB_API std::uint64_t handler_errors() const noexcept;

private:
    struct Impl;
    explicit TcpServer(std::shared_ptr<Impl> impl) noexcept;

    std::shared_ptr<Impl> impl_;
};

}  // namespace my::net
