#include "my_cpp_libs/thread/thread.hpp"

#include <chrono>
#include <exception>

namespace my::thread {

std::size_t hardware_concurrency() noexcept {
    const unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 1u : static_cast<std::size_t>(n);
}

// =============================================================================
//  ThreadPool
// =============================================================================
Result<std::unique_ptr<ThreadPool>> ThreadPool::create(std::size_t threads) {
    if (threads == 0) {
        threads = hardware_concurrency();
        if (threads > 32) threads = 32;
        if (threads == 0) threads = 1;
    }

    std::unique_ptr<ThreadPool> pool(new ThreadPool());
    pool->workers_.reserve(threads);
    try {
        for (std::size_t i = 0; i < threads; ++i) {
            pool->workers_.emplace_back([p = pool.get()] { p->worker_loop(); });
        }
    } catch (const std::exception& e) {
        // 起线程失败：把已经起来的收掉，别留 joinable thread。
        // stopping_ 必须与 worker 的 wait 使用同一把锁置位，否则会丢唤醒：
        // worker 判完谓词、还没进入 wait 时通知被丢掉，join 会永久阻塞。
        {
            std::lock_guard<std::mutex> lock(pool->mutex_);
            pool->stopping_.store(true);
        }
        pool->work_cv_.notify_all();
        for (auto& t : pool->workers_) {
            if (t.joinable()) t.join();
        }
        return Status(Errc::RuntimeError, std::string("创建线程池失败: ") + e.what());
    }
    pool->worker_count_.store(threads);
    return pool;
}

ThreadPool::~ThreadPool() { shutdown(/*drain=*/true); }

void ThreadPool::worker_loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            work_cv_.wait(lock, [this] { return stopping_.load() || !tasks_.empty(); });
            if (tasks_.empty()) return;  // stopping_ 且已排空
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
        // 顺序很重要：先跑任务体（packaged_task 在这一步兑现 promise，等 future 的
        // 线程会被唤醒），再做 pending_ 自减。所以 future::get() 返回的瞬间 pending()
        // 可能仍是 1 —— 这是有意的：pending() 表示"还在池子里（含执行中）"，
        // 而"全部完成"的屏障是 wait_idle()。头文件里写明了这个契约。
        if (pending_.fetch_sub(1) == 1) {
            // 最后一个任务结束：唤醒 wait_idle
            std::lock_guard<std::mutex> lock(mutex_);
            idle_cv_.notify_all();
        }
    }
}

void ThreadPool::shutdown(bool drain) {
    // 关键：`stopping_` 必须在**持锁**的情况下置位。
    //
    // 原来写成 `stopping_.exchange(true)` 在锁外，配合 `task = ...` 的等待方会丢唤醒：
    //   worker 持锁判谓词(假) → 释放锁 → 还没进入 pthread_cond_wait 的注册；
    //   此时 shutdown 在锁外置位 + notify_all → 通知落在"注册之前"被丢掉；
    //   worker 随后睡下，谓词虽然已经是 true，却再也没人叫醒它 → join 永久阻塞。
    // 实测约 5~25% 概率挂死（gdb 抓到 worker 停在 work_cv_.wait、主线程停在 join）。
    // 修法就是标准的条件变量用法：改状态和 notify 都要与等待方的 mutex 串行化。
    std::size_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopping_.exchange(true) && !drain) {
            dropped = tasks_.size();
            std::queue<std::function<void()>> empty;
            std::swap(tasks_, empty);
        }
    }

    if (dropped > 0) {
        // 只减掉"没开始"的那些；不能 store(0)，正在执行的任务结束还会减一次
        pending_.fetch_sub(dropped);
    }

    work_cv_.notify_all();

    // 并发/重复调用 shutdown 时，后来者要等第一个调用者 join 完。
    // 只锁 join 阶段，不能占着 mutex_ 去 join（worker 退出需要 mutex_）。
    {
        std::lock_guard<std::mutex> join_lock(join_mutex_);
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
        workers_.clear();
        worker_count_.store(0);
        std::lock_guard<std::mutex> lock(mutex_);
        idle_cv_.notify_all();
    }
}

bool ThreadPool::wait_idle(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return idle_cv_.wait_for(lock, timeout, [this] { return pending_.load() == 0; });
}
std::size_t ThreadPool::size() const noexcept { return worker_count_.load(); }
std::size_t ThreadPool::pending() const noexcept { return pending_.load(); }
bool        ThreadPool::stopping() const noexcept { return stopping_.load(); }

// =============================================================================
//  PeriodicTask
// =============================================================================
//  生命周期按"状态与对象分离"设计：
//    * 线程 lambda 只捕获 shared_ptr<State>，**不捕获 this** —— 回调里销毁任务后，
//      loop() 继续用的只有 State，不会访问已释放的 PeriodicTask（原来的 UAF）；
//    * join/detach 全部由 join_mutex_ 串行化：两个外部线程同时 stop() 时只有
//      一个能拿到 joinable 的 thread_，另一个等它 join 完后看到 !joinable；
//    * "是不是在回调线程里"用 State 里登记的 thread_id_ 判断，而不是先拿
//      join_mutex_ 再看 thread_.get_id() —— 外部线程可能正持锁 join 本线程，
//      回调线程再去等锁就是互相等死。
struct PeriodicTask::State {
    std::chrono::milliseconds  period_{0};
    std::function<void()>      fn_;
    std::mutex                 mutex_;
    std::condition_variable    cv_;
    std::atomic<bool>          stop_{false};
    std::atomic<bool>          in_loop_{false};
    std::atomic<std::uint64_t> ticks_{0};
    std::atomic<std::uint64_t> errors_{0};
    /// 周期线程自己的 id：stop() 据此判断"是不是在回调线程里"
    std::thread::id            thread_id_{};
};

Result<std::unique_ptr<PeriodicTask>> PeriodicTask::create(std::chrono::milliseconds period,
                                                           std::function<void()>  fn,
                                                           bool run_immediately) {
    if (period.count() <= 0) return Status(Errc::InvalidArgument, "周期必须为正");
    if (!fn) return Status(Errc::InvalidArgument, "回调为空");

    std::unique_ptr<PeriodicTask> task(new PeriodicTask());
    auto state     = std::make_shared<State>();
    state->period_ = period;
    state->fn_     = std::move(fn);
    task->state_   = state;

    std::promise<void> ready;
    auto               ready_future = ready.get_future();
    try {
        task->thread_ = std::thread([state, &ready, run_immediately] {
            // 先登记线程 id 并回报就绪，再跑回调：create() 返回前已经 wait 过
            // ready_future，之后任何 stop() 都能安全地看到 thread_id_。
            state->thread_id_ = std::this_thread::get_id();
            state->in_loop_.store(true);
            try {
                ready.set_value();
            } catch (...) {
            }
            if (run_immediately) {
                try {
                    state->fn_();
                    state->ticks_.fetch_add(1);
                } catch (...) {
                    state->errors_.fetch_add(1);
                }
            }
            loop(state);
        });
    } catch (const std::exception& e) {
        return Status(Errc::RuntimeError, std::string("创建周期任务失败: ") + e.what());
    }
    ready_future.wait();  // 等线程真正起来，避免 create 返回后立刻 stop 抢跑
    return task;
}

PeriodicTask::~PeriodicTask() {
    // stop() 负责两种收尾：普通线程里 join；回调线程里置停止标志并 detach。
    // 这样析构返回后 thread_ 一定不是 joinable（不会 terminate），
    // 而 detach 出去的线程由 State 的 shared_ptr 保活到 loop() 退出。
    stop();
}

void PeriodicTask::loop(const std::shared_ptr<State>& st) {
    auto next = std::chrono::steady_clock::now() + st->period_;
    std::unique_lock<std::mutex> lock(st->mutex_);
    while (!st->stop_.load()) {
        if (st->cv_.wait_until(lock, next, [&st] { return st->stop_.load(); })) break;
        lock.unlock();

        try {
            st->fn_();
            st->ticks_.fetch_add(1);
        } catch (...) {
            st->errors_.fetch_add(1);
        }

        // 从这里起一律只用 st，不碰 this —— 回调可能已经把 PeriodicTask 删掉。
        lock.lock();
        // 固定速率：下一次 = 上一次计划 + period。若已经落后太多（任务超时），
        // 直接对齐到 now + period，避免"追补式突发"把 CPU 打满。
        next += st->period_;
        const auto now = std::chrono::steady_clock::now();
        if (next < now) next = now + st->period_;
    }
}

void PeriodicTask::stop() {
    auto st = state_;
    if (!st) return;

    {
        std::lock_guard<std::mutex> lock(st->mutex_);
        st->stop_.store(true);
    }
    st->cv_.notify_all();

    // 回调线程内调用：不能 join 自己。**也不能阻塞等 join_mutex_** —— 外部线程
    // 可能正持着它在 join 本线程，等锁就是死锁。用 try_lock 串行化 detach：
    //   拿到锁 → detach（析构时 std::thread 不会因 joinable 而 terminate）；
    //   拿不到 → 说明已有别的线程在 join，交给它收尾。
    if (st->thread_id_ == std::this_thread::get_id()) {
        std::unique_lock<std::mutex> join_lock(join_mutex_, std::try_to_lock);
        if (join_lock.owns_lock() && thread_.joinable()) thread_.detach();
        return;
    }

    // 两个外部线程并发 stop()：这里串行化后只有第一个真正 join，
    // 后来者等它 join 完再看到 !joinable，不会双 join（UB）。
    std::lock_guard<std::mutex> join_lock(join_mutex_);
    if (thread_.joinable()) thread_.join();
}

std::uint64_t PeriodicTask::ticks() const noexcept {
    return state_ ? state_->ticks_.load() : 0;
}

std::uint64_t PeriodicTask::errors() const noexcept {
    return state_ ? state_->errors_.load() : 0;
}

bool PeriodicTask::running() const noexcept {
    return state_ && state_->in_loop_.load() && !state_->stop_.load();
}

}  // namespace my::thread
