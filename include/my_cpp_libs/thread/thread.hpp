#pragma once
// =============================================================================
//  my::thread —— 线程池 / 阻塞队列 / 周期任务
//
//  为什么需要它：板端是双核 A9，"采集线程 + 推理线程 + HTTP 线程"几乎是必然结构，
//  但没有这一层时每个工程都会重写一遍 `std::thread + mutex + condition_variable`，
//  而这三样正是最容易写错的地方（丢失唤醒、忘记 join、析构时 joinable → terminate）。
//
//  三条约定：
//    * **析构一定安全**：ThreadPool 析构会 drain + join；PeriodicTask 析构会 stop + join。
//      若在回调线程内销毁 PeriodicTask，则只置停止标志并 detach（不能 join 自己），
//      线程由共享 State 保活、在下一个周期点退出，之后 ticks/errors 不再更新；
//    * **有界队列优先**：BlockingQueue 默认无界，但生产端要能设上限，
//      否则"消费者慢"会变成"内存打满"（板端只有 512 MB DDR）；
//    * **不用异常上报错误**：create() 返回 Result，submit 在池已关闭时给出无效 future
//      （get() 抛 std::future_error(broken_promise)，这是 std::future 的既定语义）。
// =============================================================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "my_cpp_libs/core/status.hpp"
#include "my_cpp_libs/export.hpp"

namespace my::thread {

/// 硬件并发度；取不到时返回 1
MYLIB_API std::size_t hardware_concurrency() noexcept;

// =============================================================================
//  阻塞队列
// =============================================================================
/// capacity = 0 表示无界；close() 之后 push 立即失败、pop 把残留项取完后返回 false。
template <class T>
class BlockingQueue {
public:
    explicit BlockingQueue(std::size_t capacity = 0) : capacity_(capacity) {}

    BlockingQueue(const BlockingQueue&)            = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

    /// 入队；队列满时阻塞，关闭后返回 false
    bool push(T value) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (capacity_ > 0) {
            not_full_.wait(lock, [this] { return closed_.load() || queue_.size() < capacity_; });
        }
        if (closed_.load()) return false;
        queue_.push_back(std::move(value));
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    /// 入队；满/关闭立即返回 false
    bool try_push(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_.load()) return false;
        if (capacity_ > 0 && queue_.size() >= capacity_) return false;
        queue_.push_back(std::move(value));
        // 通知放在锁内也没问题：waiter 醒来仍需拿锁，不会丢失唤醒
        not_empty_.notify_one();
        return true;
    }

    /// 出队；关闭且已排空时返回 false
    bool pop(T& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return closed_.load() || !queue_.empty(); });
        if (queue_.empty()) return false;  // closed_ && empty
        out = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();
        not_full_.notify_one();
        return true;
    }

    /// 带超时出队；超时返回 false（并保持 out 不变）
    bool pop_timeout(T& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!not_empty_.wait_for(lock, timeout, [this] { return closed_.load() || !queue_.empty(); })) {
            return false;
        }
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();
        not_full_.notify_one();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_.store(true);
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    bool        closed() const noexcept { return closed_.load(); }
    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }
    bool empty() const { return size() == 0; }
    std::size_t capacity() const noexcept { return capacity_; }

private:
    mutable std::mutex      mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<T>           queue_;
    std::size_t             capacity_ = 0;
    std::atomic<bool>       closed_{false};
};

// =============================================================================
//  线程池
// =============================================================================
class ThreadPool {
public:
    /// threads = 0 表示按 hardware_concurrency()（至少 1，最多 32）
    MYLIB_API static Result<std::unique_ptr<ThreadPool>> create(std::size_t threads = 0);

    MYLIB_API ~ThreadPool();
    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /// 提交任务，返回 future。池已关闭时提交不会执行，future.get() 抛 broken_promise。
    template <class F, class... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using R      = std::invoke_result_t<F, Args...>;
        auto  task   = std::make_shared<std::packaged_task<R()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        auto future = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_.load()) return future;  // task 析构 → broken_promise
            tasks_.emplace([task] { (*task)(); });
            pending_.fetch_add(1);
        }
        work_cv_.notify_one();
        return future;
    }

    /// 优雅停止：drain=true 先跑完队列，false 直接丢弃未开始的任务（两者都会 join）
    MYLIB_API void shutdown(bool drain = true);
    /// 等队列清空且任务都跑完；超时返回 false。
    /// **这是"全部完成"的唯一可靠屏障**：`future::get()` 只保证任务体已返回，
    /// 而 pending() 要等 packaged_task 兑现 promise 之后才自减，两者之间有一个极小窗口。
    MYLIB_API bool wait_idle(std::chrono::milliseconds timeout);

    /// 当前存活 worker 数；shutdown 之后为 0
    MYLIB_API std::size_t size() const noexcept;
    /// 已提交但未完成的任务数（含正在执行的）。
    /// 注意：它降到 0 的时刻**略晚于**最后一个 future 就绪的时刻（见 wait_idle 的说明），
    /// 想判断"都跑完了"请用 wait_idle()。
    MYLIB_API std::size_t pending() const noexcept;
    MYLIB_API bool        stopping() const noexcept;

private:
    ThreadPool() = default;
    void worker_loop();

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex                mutex_;
    /// 串行化 shutdown 的 join 阶段：并发调用时后来者要等第一个 join 完才能返回
    std::mutex                        join_mutex_;
    std::condition_variable           work_cv_;
    std::condition_variable           idle_cv_;
    std::atomic<bool>                 stopping_{false};
    std::atomic<std::size_t>          pending_{0};
    /// 单独存一份：shutdown() 会清空 workers_，而 size() 可能被别的线程同时调
    std::atomic<std::size_t>          worker_count_{0};
};

// =============================================================================
//  周期任务
// =============================================================================
/// 固定周期回调（周期从"上一次开始"算，不会因为执行慢而漂移成突发）。
/// 回调抛异常会被吞掉并计数，不会把线程带走。
///
/// 生命周期：状态全部放在 shared_ptr<State> 里，线程只捕获它、不再捕获 this。
/// 因此：
///   * stop() 可从任意线程重复调用；并发调用由内部 join_mutex_ 串行化 ——
///     两个外部线程同时 stop() 也只会 join 一次，不会对同一个 std::thread 双 join；
///   * 在回调线程内 stop()/销毁任务时只置停止标志并 detach（不能 join 自己）。
///     此时 State 由线程持有的 shared_ptr 保活，loop() 不会再访问已销毁的
///     PeriodicTask，结构上安全；但线程要在下一个周期点才真正退出，
///     之后 ticks/errors/running 不再更新。
///   * 析构本身可从任意线程（包括回调线程）调用；但**不要与其他线程正在进行的
///     stop()/running() 等成员调用并发** —— 对象销毁与成员调用并发属于调用方的
///     生命周期错误（任何 C++ 对象都是如此）。
class PeriodicTask {
public:
    /// period 必须 > 0；run_immediately=true 时先立刻执行一次再进入周期
    MYLIB_API static Result<std::unique_ptr<PeriodicTask>> create(std::chrono::milliseconds period,
                                                                 std::function<void()>  fn,
                                                                 bool run_immediately = false);

    MYLIB_API ~PeriodicTask();
    PeriodicTask(const PeriodicTask&)            = delete;
    PeriodicTask& operator=(const PeriodicTask&) = delete;

    /// 停止并 join。可从任意线程重复调用；并发调用由内部 join_mutex_ 串行化。
    /// 回调线程内调用只置停止标志并 detach（不 join 自己），此时返回不代表线程已退出。
    MYLIB_API void stop();

    MYLIB_API std::uint64_t ticks() const noexcept;
    MYLIB_API std::uint64_t errors() const noexcept;
    MYLIB_API bool          running() const noexcept;

private:
    PeriodicTask() = default;

    /// 与 PeriodicTask 对象解耦的生命周期状态：线程只碰它，不碰 this。
    /// 这样"在回调里销毁任务"之后 loop() 仍然安全（原先会让线程继续访问 this）。
    struct State;
    static void loop(const std::shared_ptr<State>& st);

    std::shared_ptr<State> state_;
    std::thread            thread_;
    /// 串行化 joinable()/join()/detach()：并发 stop() 时后来者必须等第一个 join 完
    std::mutex             join_mutex_;
};

}  // namespace my::thread
