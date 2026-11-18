// =============================================================================
//  thread 组件：线程池 / 阻塞队列 / 周期任务
//
//  这些用例是 TSan job 的主要食粮：所有"文档声称线程安全"的地方都必须在这里
//  被真正并发跑一遍。
// =============================================================================
#include "my_cpp_libs/thread/thread.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <vector>

#include "test_framework.hpp"

MYTEST_SUITE("thread");

MYTEST_CASE(thread_hardware_concurrency_is_sane) {
    const auto n = my::thread::hardware_concurrency();
    CHECK(n >= 1);
    CHECK(n <= 1024);
}

MYTEST_CASE(thread_pool_runs_all_tasks) {
    auto made = my::thread::ThreadPool::create(4);
    CHECK_RESULT(made);
    auto pool = std::move(made.value());
    CHECK_EQ(pool->size(), std::size_t{4});

    std::vector<std::future<int>> futures;
    futures.reserve(32);
    for (int i = 0; i < 32; ++i) {
        futures.push_back(pool->submit([i] { return i * i; }));
    }

    int sum = 0;
    for (auto& f : futures) sum += f.get();
    // sum(i^2, i=0..31) = 31*32*63/6 = 10416
    CHECK_EQ(sum, 10416);
    CHECK_EQ(pool->pending(), std::size_t{0});
}

MYTEST_CASE(thread_pool_runs_concurrently) {
    auto made = my::thread::ThreadPool::create(4);
    CHECK_RESULT(made);
    auto pool = std::move(made.value());

    std::atomic<int> counter{0};
    for (int i = 0; i < 200; ++i) {
        (void)pool->submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
    }
    CHECK(pool->wait_idle(std::chrono::milliseconds(5000)));
    CHECK_EQ(counter.load(), 200);
}

MYTEST_CASE(thread_pool_wait_idle_times_out_when_busy) {
    auto made = my::thread::ThreadPool::create(1);
    CHECK_RESULT(made);
    auto pool = std::move(made.value());

    // 用阻塞队列卡住唯一的工作线程（有界等待：即使主线程提前返回也不会永久卡住）
    my::thread::BlockingQueue<int> gate;
    std::atomic<bool>              worker_started{false};
    std::atomic<bool>              worker_left{false};
    (void)pool->submit([&] {
        worker_started.store(true);
        int dummy = 0;
        (void)gate.pop_timeout(dummy, std::chrono::seconds(5));
        worker_left.store(true);
    });

    // 等 worker 真的进到任务里，避免"任务还没开始"导致的假阴性
    for (int i = 0; i < 500 && !worker_started.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(worker_started.load());

    // 先判定，**无论如何都放行**，再断言（否则断言失败时 worker 会白等 5 秒）
    const bool idle_too_soon = pool->wait_idle(std::chrono::milliseconds(50));
    gate.close();  // 唤醒 worker
    CHECK(!idle_too_soon);
    CHECK(pool->wait_idle(std::chrono::milliseconds(5000)));
    CHECK(worker_left.load());
}

MYTEST_CASE(thread_pool_shutdown_drains_by_default) {
    std::atomic<int> done{0};
    {
        auto made = my::thread::ThreadPool::create(2);
        CHECK_RESULT(made);
        auto pool = std::move(made.value());
        for (int i = 0; i < 50; ++i) {
            (void)pool->submit([&done] { done.fetch_add(1, std::memory_order_relaxed); });
        }
        pool->shutdown(/*drain=*/true);
        CHECK(pool->stopping());
    }
    CHECK_EQ(done.load(), 50);  // drain 必须把队列跑完
}

MYTEST_CASE(thread_pool_submit_after_shutdown_breaks_promise) {
    auto made = my::thread::ThreadPool::create(1);
    CHECK_RESULT(made);
    auto pool = std::move(made.value());
    pool->shutdown();

    auto f = pool->submit([] { return 1; });
    bool threw = false;
    try {
        (void)f.get();
    } catch (const std::future_error&) {
        threw = true;  // 既定语义：任务不会被执行，future 里是 broken_promise
    }
    CHECK(threw);
}

MYTEST_CASE(thread_pool_create_with_zero_uses_hardware) {
    auto made = my::thread::ThreadPool::create(0);
    CHECK_RESULT(made);
    auto pool = std::move(made.value());
    const auto hw = my::thread::hardware_concurrency();
    CHECK_EQ(pool->size(), hw > 32 ? std::size_t{32} : hw);
}

MYTEST_CASE(thread_blocking_queue_fifo) {
    my::thread::BlockingQueue<int> q;
    CHECK(q.push(1));
    CHECK(q.push(2));
    CHECK_EQ(q.size(), std::size_t{2});

    int v = 0;
    CHECK(q.pop(v));
    CHECK_EQ(v, 1);
    CHECK(q.pop(v));
    CHECK_EQ(v, 2);
    CHECK_EQ(q.size(), std::size_t{0});
}

MYTEST_CASE(thread_blocking_queue_bounded_blocks_producer) {
    my::thread::BlockingQueue<int> q(2);
    CHECK(q.push(1));
    CHECK(q.push(2));
    // 满了：try_push 立即失败
    CHECK(!q.try_push(3));

    std::atomic<bool> pushed{false};
    std::thread       producer([&] {
        if (q.push(3)) pushed.store(true);  // 这里会阻塞到消费者腾出位置
    });

    int v = 0;
    CHECK(q.pop(v));
    CHECK_EQ(v, 1);
    producer.join();
    CHECK(pushed.load());
    CHECK_EQ(q.size(), std::size_t{2});  // 2 和 3
}

MYTEST_CASE(thread_blocking_queue_close_wakes_waiters) {
    my::thread::BlockingQueue<int> q;
    std::atomic<bool>              popped{true};

    std::thread consumer([&] {
        int v = 0;
        popped.store(q.pop(v));  // 应该在 close 之后返回 false
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    q.close();
    consumer.join();

    CHECK(!popped.load());
    CHECK(q.closed());
    CHECK(!q.push(1));  // 关闭后 push 立即失败
}

MYTEST_CASE(thread_blocking_queue_pop_timeout) {
    my::thread::BlockingQueue<int> q;
    int                            v = 0;
    const auto                     t0 = std::chrono::steady_clock::now();
    CHECK(!q.pop_timeout(v, std::chrono::milliseconds(30)));
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(elapsed >= std::chrono::milliseconds(20));  // 真的等了
    CHECK(elapsed < std::chrono::milliseconds(2000));

    // 有数据时立刻成功
    CHECK(q.push(7));
    CHECK(q.pop_timeout(v, std::chrono::milliseconds(50)));
    CHECK_EQ(v, 7);
}

MYTEST_CASE(thread_periodic_task_ticks) {
    std::atomic<int> count{0};
    auto made = my::thread::PeriodicTask::create(std::chrono::milliseconds(10),
                                                 [&count] { count.fetch_add(1); });
    CHECK_RESULT(made);
    auto task = std::move(made.value());

    // 10ms 周期，等 120ms：至少 2 次（留足调度余量，不追求精确次数）
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    task->stop();

    CHECK(count.load() >= 2);
    CHECK_EQ(static_cast<int>(task->ticks()), count.load());
    CHECK(!task->running());
    // 停止之后计数不能继续涨
    const int after = count.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_EQ(count.load(), after);
}

MYTEST_CASE(thread_periodic_task_run_immediately) {
    std::atomic<int> count{0};
    auto made = my::thread::PeriodicTask::create(std::chrono::milliseconds(1000),
                                                 [&count] { count.fetch_add(1); },
                                                 /*run_immediately=*/true);
    CHECK_RESULT(made);
    auto task = std::move(made.value());

    // 周期是 1 秒，但 run_immediately 应该让它马上跑一次
    for (int i = 0; i < 100 && count.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK_EQ(count.load(), 1);
    task->stop();
}

MYTEST_CASE(thread_periodic_task_survives_throwing_callback) {
    std::atomic<int> calls{0};
    auto made = my::thread::PeriodicTask::create(std::chrono::milliseconds(10), [&calls] {
        calls.fetch_add(1);
        throw 1;
    });
    CHECK_RESULT(made);
    auto task = std::move(made.value());

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    task->stop();

    CHECK(calls.load() >= 2);            // 抛异常也要继续周期执行
    CHECK(task->errors() >= std::uint64_t{2});           // 且被计入 errors
    CHECK_EQ(static_cast<std::uint64_t>(task->ticks()), std::uint64_t{0});  // 成功的 tick 是 0
}

MYTEST_CASE(thread_periodic_task_rejects_bad_args) {
    CHECK(!my::thread::PeriodicTask::create(std::chrono::milliseconds(0), [] {}));
    CHECK(!my::thread::PeriodicTask::create(std::chrono::milliseconds(10), nullptr));
}

MYTEST_CASE(thread_periodic_task_stop_from_callback) {
    // 在回调内部 stop() 不能死锁/自 join。
    //
    // 指针必须是 atomic：主线程写、回调线程读，普通指针就是数据竞争
    // （TSan 抓到过 —— 这条用例自己踩了"并发要同步"的坑）。
    std::atomic<my::thread::PeriodicTask*> raw{nullptr};
    auto made = my::thread::PeriodicTask::create(std::chrono::milliseconds(10), [&] {
        auto* self = raw.load(std::memory_order_acquire);
        if (self != nullptr) self->stop();
    });
    CHECK_RESULT(made);
    auto task = std::move(made.value());
    raw.store(task.get(), std::memory_order_release);

    // 等第一次 tick 触发自我 stop
    for (int i = 0; i < 500 && task->running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(!task->running());
    task->stop();  // 幂等（这次从主线程 join，把 detach 的尾巴收掉）
}
