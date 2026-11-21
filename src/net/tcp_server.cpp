// =============================================================================
//  my::net::TcpServer 实现
//
//  形状：Impl（跨线程共享状态）+ 1 个 accept 线程 + 每连接 1 个 detached 线程。
//
//  为什么状态全在 Impl、由 shared_ptr 持有：
//    handler 跑在独立线程里，而 TcpServer 对象可能先被析构。线程只碰 Impl、不碰
//    TcpServer，于是"对象没了、线程还在收尾"不会变成 UAF；最后一个 shared_ptr
//    释放时 listener 才真正关闭。
//
//  为什么 accept 用轮询而不是 close 打断：
//    一个线程 select()、另一个线程 closesocket() 同一个 fd，属于对同一 fd 的并发
//    访问（数据竞争），Windows 上关闭后的句柄号还可能立刻被回收给别的 socket。
//    这里给 accept 带 accept_timeout_ms 超时，stop() 只要等这一轮返回即可；关闭
//    listener 也放在 listener_mtx 里做，保证不会和 accept() 并发 —— 代价是
//    stop() 的最坏延迟 ≈ accept_timeout_ms。
//
//  为什么连接线程是 detached：
//    handler 可能永远不返回（用户配了 idle_timeout_ms = 0 又卡在阻塞 read 上）。
//    若 join，stop()/析构就会被一个坏业务卡死。detached + shared_ptr<Impl> 让
//    stop() 变成"有界等待"：最多 5 s，等不到就不等了（线程自己跑完、自己收尾）。
//
//  -fno-exceptions 构建（-DMYLIB_NO_EXCEPTIONS=1）：本文件退化成桩实现，见文件
//  末尾的 #else 分支 —— 头文件里的类型照常存在，只是 create()/start() 报
//  Unsupported，避免消费方在链接期才发现符号缺失。
// =============================================================================

#include "my_cpp_libs/net/tcp_server.hpp"

#if defined(MYLIB_NO_EXCEPTIONS)

// =============================================================================
//  -fno-exceptions 构建：桩实现
//
//  TcpServer 需要 std::thread + 异常边界（handler 抛异常要吞掉、线程创建失败要
//  翻译成 Status），这些在 -fno-exceptions 下没有意义。所以这里保留全部符号、
//  返回明确的 Unsupported：消费方代码照常编译链接，只有在真的要用 TcpServer 时
//  才会在运行/构造阶段拿到"本构建不支持"的错误，而不是一个链接错误。
// =============================================================================

namespace my::net {

// 空 Impl：noex 构建里不可能真的构造出 TcpServer（create 永远失败），这里给出
// 完整定义只是为了让 shared_ptr<Impl> 的析构/move 有完整的类型，语义清楚。
struct TcpServer::Impl {};

TcpServer::TcpServer(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
TcpServer::~TcpServer()                              = default;
TcpServer::TcpServer(TcpServer&&) noexcept           = default;
TcpServer& TcpServer::operator=(TcpServer&&) noexcept = default;

Result<TcpServer> TcpServer::create(const Endpoint&, Handler, const TcpServerOptions&) {
    return Status(Errc::Unsupported,
                  "TcpServer::create: 本构建是 -fno-exceptions（MYLIB_NO_EXCEPTIONS），"
                  "TcpServer 需要 std::thread 与异常边界，不可用");
}

Status TcpServer::start() {
    return Status(Errc::Unsupported,
                  "TcpServer::start: 本构建是 -fno-exceptions（MYLIB_NO_EXCEPTIONS），"
                  "TcpServer 不可用");
}

void TcpServer::stop() noexcept {}

bool TcpServer::running() const noexcept { return false; }

std::uint16_t TcpServer::port() const { return 0; }

std::size_t TcpServer::active_connections() const noexcept { return 0; }

std::uint64_t TcpServer::accepted_total() const noexcept { return 0; }

std::uint64_t TcpServer::rejected_total() const noexcept { return 0; }

std::uint64_t TcpServer::handler_errors() const noexcept { return 0; }

}  // namespace my::net

#else  // 正常构建：完整实现

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>

namespace my::net {
namespace {

/// stop() 等在途连接退出的上限。理由见文件头：handler 可能永远不返回，
/// 停止路径必须有界。
constexpr auto kStopWaitForConnections = std::chrono::seconds(5);

}  // namespace

// =============================================================================
//  Impl：所有跨线程共享的状态
// =============================================================================
struct TcpServer::Impl {
    // --- 配置：create() 之后只读，线程可以无锁读 ---
    TcpServerOptions           options;
    TcpServer::Handler         handler;
    Endpoint                   bound_ep;  ///< 实际绑定到的地址（port 已换成真实端口）
    std::atomic<std::uint16_t> port{0};

    // --- listener ---
    // 只被 accept 线程与 stop() 使用；独立互斥量串行化，避免"一个线程在
    // select(accept) 里、另一个线程 close 同一个 fd"的数据竞争。
    std::mutex  listener_mtx;
    TcpListener listener;

    // --- 生命周期 ---
    // start()/stop() 用它串行化。accept 线程**不**碰这把锁，所以 stop() 可以
    // 持锁 join accept 线程（不会死锁）。
    std::mutex  ctl_mtx;
    std::thread accept_thread;

    /// accept 循环每个轮询周期检查一次；false 时尽快退出
    std::atomic<bool> running{false};

    // --- 在途连接计数 + "都退出了"的通知 ---
    std::mutex               conn_mtx;
    std::condition_variable  conn_cv;
    std::atomic<std::size_t> active{0};

    // --- 统计（调用方 / accept 线程 / 连接线程三方并发） ---
    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> handler_errors{0};

    /// 取一个连接；stop() 期间返回 Cancelled
    Result<TcpSocket> accept_once();

    /// accept 线程主体（self 是为了在 detached 线程里保命）
    void accept_loop(std::shared_ptr<Impl> self);

    /// 上限判断 + 起连接线程
    void spawn_connection(std::shared_ptr<Impl> self, TcpSocket sock);

    /// 连接线程主体：跑 handler、计数、关连接
    static void connection_main(std::shared_ptr<Impl> self, TcpSocket sock, Endpoint peer);

    Status start_impl(const std::shared_ptr<Impl>& self);
    void   stop_impl();
};

// =============================================================================
//  accept 线程
// =============================================================================
Result<TcpSocket> TcpServer::Impl::accept_once() {
    std::lock_guard<std::mutex> lk(listener_mtx);
    if (!running.load(std::memory_order_acquire)) {
        return Status(Errc::Cancelled, "TcpServer: 正在停止，不再 accept");
    }
    return listener.accept(options.accept_timeout_ms);
}

void TcpServer::Impl::accept_loop(std::shared_ptr<Impl> self) {
    for (;;) {
        if (!self->running.load(std::memory_order_acquire)) break;

        Result<TcpSocket> conn = self->accept_once();
        if (!conn) {
            const Errc code = conn.status().code();
            if (code == Errc::Timeout) continue;    // 正常轮询：没有新连接
            if (code == Errc::Cancelled) continue;  // stop() 已置 running=false，下一轮退出
            if (!self->running.load(std::memory_order_acquire)) break;  // stop() 关 listener 导致的错误
            // 真正的意外（listener 被外部破坏等）：running 仍为 true 时置 stopped 并
            // 退出，让调用方从 running()==false 察觉；再 start() 会重新 bind。
            self->running.store(false, std::memory_order_release);
            break;
        }

        if (!self->running.load(std::memory_order_acquire)) {
            conn.value().close();  // 停止过程中接到的连接：不交给 handler，直接丢
            break;
        }
        self->spawn_connection(self, std::move(conn.value()));
    }
}

void TcpServer::Impl::spawn_connection(std::shared_ptr<Impl> self, TcpSocket sock) {
    const Endpoint peer = sock.peer();  // 先取对端地址：之后 socket 会被移进线程

    // active 只由 accept 线程加（连接线程只减），load + fetch_add 之间不需要锁。
    if (self->active.load(std::memory_order_relaxed) >= self->options.max_connections) {
        self->rejected.fetch_add(1, std::memory_order_relaxed);
        sock.close();  // 超过上限：直接关，客户端读到的就是 0（FIN）
        return;
    }
    self->active.fetch_add(1, std::memory_order_relaxed);

    // 每连接收超时。失败也继续跑：最坏是 handler 阻塞在 recv 上，
    // 而那是 idle_timeout_ms == 0 时调用方自己选的语义。
    (void)sock.set_timeout(self->options.idle_timeout_ms);

    try {
        std::thread worker([self, sock = std::move(sock), peer]() mutable {
            Impl::connection_main(std::move(self), std::move(sock), std::move(peer));
        });
        // detached：线程的寿命由 shared_ptr<Impl> + 自己手里的 socket 保证，
        // 不挂在 TcpServer 对象（或 Impl）的生命周期上。
        worker.detach();
    } catch (...) {
        // 线程创建失败（EAGAIN / 内存不足）：把 active 还回去。socket 已随 lambda
        // 析构关闭。这种连接既没进 handler、也不是被上限拒的，所以两个计数都不加。
        // 不需要 notify：stop() 要先 join 完 accept 线程才会等 active==0，
        // 此刻它不可能已经在等。
        self->active.fetch_sub(1, std::memory_order_relaxed);
    }
}

void TcpServer::Impl::connection_main(std::shared_ptr<Impl> self, TcpSocket sock, Endpoint peer) {
    // 计数放在 handler **之前**：客户端拿到回包时 accepted_total() 一定能看到这次
    // 连接，不会出现"业务已经回包、统计还少一条"的中间状态。
    self->accepted.fetch_add(1, std::memory_order_relaxed);

    try {
        self->handler(sock, peer);
    } catch (...) {
        // handler 的异常不能吃掉整个进程：计一条，然后当作连接正常结束。
        self->handler_errors.fetch_add(1, std::memory_order_relaxed);
    }

    sock.close();  // handler 返回后连接一定被关，不等析构
    {
        // 在 conn_mtx 里减 active：和 stop() 的 wait_for 谓词用同一把锁，保证
        // "已经减到 0"对 stop() 立即可见（不会白等满 5 s）。
        std::lock_guard<std::mutex> lk(self->conn_mtx);
        self->active.fetch_sub(1, std::memory_order_relaxed);
    }
    self->conn_cv.notify_all();
}

// =============================================================================
//  启动 / 停止
// =============================================================================
Status TcpServer::Impl::start_impl(const std::shared_ptr<Impl>& self) {
    std::lock_guard<std::mutex> ctl(ctl_mtx);
    if (running.load(std::memory_order_acquire)) {
        return Status(Errc::Busy, "TcpServer::start: 已经在运行");
    }

    // 上一个 accept 线程若不是被 stop() 收掉的（例如 accept 出致命错误自己退出），
    // 这里必须先收尾：对仍 joinable 的 std::thread 做 move 赋值会直接 std::terminate。
    if (accept_thread.joinable()) {
        if (accept_thread.get_id() == std::this_thread::get_id()) {
            accept_thread.detach();
        } else {
            accept_thread.join();
        }
    }

    {
        std::lock_guard<std::mutex> lk(listener_mtx);
        if (!listener.valid()) {
            // stop() 把 listener 关了：按 create() 时**实际**绑定的端口重新 bind，
            // 这样同一个对象可以反复 start/stop（而不是一次性的）。
            auto rebound = TcpListener::bind(bound_ep, options.backlog, options.reuse_addr);
            if (!rebound) {
                std::string msg = "TcpServer::start: 重新 bind 失败（";
                msg += rebound.status().message();
                msg += "）";
                return Status(rebound.status().code(), std::move(msg));
            }
            listener = std::move(rebound.value());
            port.store(listener.port(), std::memory_order_relaxed);
        }
    }

    running.store(true, std::memory_order_release);
    try {
        accept_thread = std::thread([self]() { self->accept_loop(self); });
    } catch (const std::exception& e) {
        running.store(false, std::memory_order_release);
        return Status(Errc::RuntimeError,
                      std::string("TcpServer::start: 创建 accept 线程失败: ") + e.what());
    } catch (...) {
        running.store(false, std::memory_order_release);
        return Status(Errc::RuntimeError,
                      "TcpServer::start: 创建 accept 线程失败（未知错误）");
    }
    return Ok();
}

void TcpServer::Impl::stop_impl() {
    // 持锁做完整段停止：start/stop 之间不交叉。accept 线程不碰这把锁，所以
    // 下面 join 不会死锁。
    std::unique_lock<std::mutex> ctl(ctl_mtx);
    running.store(false, std::memory_order_release);

    {
        // 关 listener：端口立刻释放；正在 accept 里等待的那一轮会在超时/出错后
        // 返回（最坏 accept_timeout_ms），accept 线程随即看到 running==false 退出。
        std::lock_guard<std::mutex> lk(listener_mtx);
        listener.close();
    }

    if (accept_thread.joinable()) {
        if (accept_thread.get_id() == std::this_thread::get_id()) {
            // 从 accept 线程里调 stop()：join 自己会抛，只能 detach。
            // （正常路径不可达：用户代码碰不到 accept 线程；连接线程里调 stop()
            //   走的是下面的 join 分支。）
            accept_thread.detach();
        } else {
            accept_thread.join();  // 最坏 ≈ accept_timeout_ms
        }
    }

    // 等在途连接退出：有界 5 s。等不到就不再等 —— 连接线程是 detached 的，
    // 它继续把 handler 跑完、自己关 socket，shared_ptr<Impl> 保证状态还活着。
    std::unique_lock<std::mutex> lk(conn_mtx);
    conn_cv.wait_for(lk, kStopWaitForConnections, [this] {
        return active.load(std::memory_order_relaxed) == 0;
    });
}

// =============================================================================
//  TcpServer 公开接口
// =============================================================================
TcpServer::TcpServer(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

TcpServer::~TcpServer() { stop(); }

TcpServer::TcpServer(TcpServer&& other) noexcept : impl_(std::move(other.impl_)) {}

TcpServer& TcpServer::operator=(TcpServer&& other) noexcept {
    if (this != &other) {
        // 旧 Impl 里可能还有 accept 线程：必须先停，否则旧线程没人 join、
        // 旧端口也不会释放（impl_ 被覆盖后那个 Impl 就没人认识了）。
        stop();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

Result<TcpServer> TcpServer::create(const Endpoint& ep, Handler handler,
                                    const TcpServerOptions& options) {
    // ---- 参数校验：这些是调用方的编程错误，create() 阶段就拒绝，不拖到 start() ----
    if (!handler) {
        return Status(Errc::InvalidArgument, "TcpServer::create: handler 不能为空");
    }
    if (options.max_connections == 0) {
        return Status(Errc::InvalidArgument,
                      "TcpServer::create: max_connections 必须 >= 1（0 不做'不限制'语义）");
    }
    if (options.idle_timeout_ms < 0) {
        return Status(Errc::InvalidArgument,
                      "TcpServer::create: idle_timeout_ms 不能为负（0 表示阻塞）");
    }
    if (options.accept_timeout_ms < 0) {
        return Status(Errc::InvalidArgument,
                      "TcpServer::create: accept_timeout_ms 不能为负");
    }

    try {
        TcpServerOptions normalized = options;
        if (normalized.backlog <= 0) normalized.backlog = 1;  // listen(0) 的队列语义各平台不一
        if (normalized.accept_timeout_ms == 0) {
            // select(0) 会立刻超时 → accept 循环变成忙轮询。按 1 ms 处理，
            // 仍然是"尽量快停下"的语义，但不烧 CPU。
            normalized.accept_timeout_ms = 1;
        }

        // 先 bind：端口冲突 / 非法地址在这里就暴露（而不是 start() 之后才发现）。
        auto listener = TcpListener::bind(ep, normalized.backlog, normalized.reuse_addr);
        if (!listener) return listener.status();
        const std::uint16_t actual_port = listener.value().port();

        auto impl = std::make_shared<Impl>();
        impl->options = normalized;
        // std::function 的堆分配发生在调用方构造 Handler 的时候；这里的 move 不分配。
        // 但仍然由下面的 catch 兜住 Impl/handler 内部的任何分配失败。
        impl->handler = std::move(handler);
        impl->port.store(actual_port, std::memory_order_relaxed);
        // 重启用**实际**端口：ep.port 为 0 时系统分配的端口必须记下来。
        impl->bound_ep = Endpoint{ep.host, actual_port};
        impl->listener = std::move(listener.value());

        return TcpServer(std::move(impl));
    } catch (const std::bad_alloc&) {
        return Status(Errc::RuntimeError, "TcpServer::create: 内存不足");
    } catch (const std::exception& e) {
        return Status(Errc::RuntimeError, std::string("TcpServer::create: ") + e.what());
    }
}

Status TcpServer::start() {
    if (!impl_) {
        return Status(Errc::NotInitialized,
                      "TcpServer::start: 对象未初始化（create() 失败或已被移动）");
    }
    // 传一份 shared_ptr 进线程：线程的存活不依赖 TcpServer 对象。
    const std::shared_ptr<Impl> self = impl_;
    return self->start_impl(self);
}

void TcpServer::stop() noexcept {
    if (!impl_) return;
    try {
        impl_->stop_impl();
    } catch (...) {
        // stop() 是 noexcept（还会在析构路径上被调用）：mutex/CV 抛出的
        // system_error 实际不会发生，这里吞掉是为了不把析构变成 std::terminate。
    }
}

bool TcpServer::running() const noexcept {
    return impl_ && impl_->running.load(std::memory_order_acquire);
}

std::uint16_t TcpServer::port() const {
    return impl_ ? impl_->port.load(std::memory_order_relaxed) : 0;
}

std::size_t TcpServer::active_connections() const noexcept {
    return impl_ ? impl_->active.load(std::memory_order_relaxed) : 0;
}

std::uint64_t TcpServer::accepted_total() const noexcept {
    return impl_ ? impl_->accepted.load(std::memory_order_relaxed) : 0;
}

std::uint64_t TcpServer::rejected_total() const noexcept {
    return impl_ ? impl_->rejected.load(std::memory_order_relaxed) : 0;
}

std::uint64_t TcpServer::handler_errors() const noexcept {
    return impl_ ? impl_->handler_errors.load(std::memory_order_relaxed) : 0;
}

}  // namespace my::net

#endif  // MYLIB_NO_EXCEPTIONS
