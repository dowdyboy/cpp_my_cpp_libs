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
        // 起线程失败：把已经起来的收掉，别留 joinable thread
        pool->stopping_.store(true);
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
        if (stopping_.exchange(true)) return;  // 幂等
        if (!drain) {
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

    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    idle_cv_.notify_all();
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
Result<std::unique_ptr<PeriodicTask>> PeriodicTask::create(std::chrono::milliseconds period,
                                                           std::function<void()>  fn,
                                                           bool run_immediately) {
    if (period.count() <= 0) return Status(Errc::InvalidArgument, "周期必须为正");
    if (!fn) return Status(Errc::InvalidArgument, "回调为空");

    std::unique_ptr<PeriodicTask> task(new PeriodicTask());
    task->period_ = period;
    task->fn_     = std::move(fn);

    std::promise<void> ready;
    auto               ready_future = ready.get_future();
    try {
        task->thread_ = std::thread([p = task.get(), &ready, run_immediately] {
            p->in_loop_.store(true);
            try {
                ready.set_value();
            } catch (...) {
            }
            if (run_immediately) {
                try {
                    p->fn_();
                    p->ticks_.fetch_add(1);
                } catch (...) {
                    p->errors_.fetch_add(1);
                }
            }
            p->loop();
        });
    } catch (const std::exception& e) {
        return Status(Errc::RuntimeError, std::string("创建周期任务失败: ") + e.what());
    }
    ready_future.wait();  // 等线程真正起来，避免 create 返回后立刻 stop 抢跑
    return task;
}

PeriodicTask::~PeriodicTask() {
    stop();
    if (thread_.joinable()) {
        // 只有"在回调里销毁自己"才会走到这里（这种用法本身不合法）。
        // terminate 太狠，退化成 detach，并把禁止事项写进头文件注释。
        thread_.detach();
    }
}

void PeriodicTask::loop() {
    auto next = std::chrono::steady_clock::now() + period_;
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_.load()) {
        if (cv_.wait_until(lock, next, [this] { return stop_.load(); })) break;
        lock.unlock();

        try {
            fn_();
            ticks_.fetch_add(1);
        } catch (...) {
            errors_.fetch_add(1);
        }

        lock.lock();
        // 固定速率：下一次 = 上一次计划 + period。若已经落后太多（任务超时），
        // 直接对齐到 now + period，避免"追补式突发"把 CPU 打满。
        next += period_;
        const auto now = std::chrono::steady_clock::now();
        if (next < now) next = now + period_;
    }
}

void PeriodicTask::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_.store(true);
    }
    cv_.notify_all();

    if (!thread_.joinable()) return;
    if (std::this_thread::get_id() == thread_.get_id()) {
        // 在回调内部调 stop()：不能 join 自己。**也不能 detach** ——
        // detach 之后对象可能先被销毁而线程还在跑（use-after-free）。
        // 保持 joinable，交给之后从别的线程来的 stop()/析构去 join。
        return;
    }
    thread_.join();
}


std::uint64_t PeriodicTask::ticks() const noexcept { return ticks_.load(); }
std::uint64_t PeriodicTask::errors() const noexcept { return errors_.load(); }
bool          PeriodicTask::running() const noexcept { return in_loop_.load() && !stop_.load(); }

}  // namespace my::thread
