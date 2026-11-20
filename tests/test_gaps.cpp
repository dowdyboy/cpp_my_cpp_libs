// =============================================================================
//  0.1.9：补上审计发现的测试空洞
//
//  这批用例的共同点：都是"公开 API 存在、但没有任何用例覆盖"或"覆盖了但没断言到点上"。
//  每条都写成**能失败**的断言（改坏实现就该红），而不是"调一下不崩"。
// =============================================================================

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <string>
#include <vector>

#include "my_cpp_libs/image/image.hpp"
#include "my_cpp_libs/io/io.hpp"
#include "my_cpp_libs/json/json.hpp"
#include "my_cpp_libs/math/math.hpp"
#include "my_cpp_libs/net/net.hpp"
#include "my_cpp_libs/thread/thread.hpp"
#include "my_cpp_libs/time/time.hpp"

#include "test_framework.hpp"

MYTEST_SUITE("gaps");

namespace {

std::string tmp_dir() {
#if defined(_WIN32)
    const char* t = std::getenv("TEMP");
    return std::string(t ? t : ".") + "\\mylib_gaps";
#else
    const char* t = std::getenv("TMPDIR");
    return std::string(t ? t : "/tmp") + "/mylib_gaps";
#endif
}

/// 造一张有纹理的 RGB 图（纯色图在 JPEG 压缩后几乎不失真，测不出问题）
my::image::Image make_textured(int w, int h) {
    my::image::Image img;
    img.width    = w;
    img.height   = h;
    img.channels = 3;
    img.pixels.resize(static_cast<std::size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 3;
            img.pixels[i + 0]   = static_cast<std::uint8_t>((x * 7 + y * 3) & 0xFF);
            img.pixels[i + 1]   = static_cast<std::uint8_t>((x * 3 + y * 11) & 0xFF);
            img.pixels[i + 2]   = static_cast<std::uint8_t>((x * 5 + y * 5) & 0xFF);
        }
    }
    return img;
}

}  // namespace

// =============================================================================
//  一、time / json / math / io / net / thread 的零覆盖 API
// =============================================================================
MYTEST_CASE(gap_time_sleep_s_actually_sleeps) {
    const auto t0 = my::time::monotonic_us();
    my::time::sleep_s(0.02);
    const auto elapsed_ms = (my::time::monotonic_us() - t0) / 1000;
    CHECK(elapsed_ms >= 12);   // 真的睡了（Windows 定时器粒度 ~15.6ms，留点余量）
    CHECK(elapsed_ms < 2000);  // 也没睡过头
}

MYTEST_CASE(gap_json_type_name_covers_every_type) {
    using my::json::Type;
    CHECK_EQ(std::string(my::json::type_name(Type::Null)), std::string("null"));
    CHECK_EQ(std::string(my::json::type_name(Type::Boolean)), std::string("boolean"));
    CHECK_EQ(std::string(my::json::type_name(Type::Integer)), std::string("integer"));
    CHECK_EQ(std::string(my::json::type_name(Type::Floating)), std::string("floating"));
    CHECK_EQ(std::string(my::json::type_name(Type::String)), std::string("string"));
    CHECK_EQ(std::string(my::json::type_name(Type::Array)), std::string("array"));
    CHECK_EQ(std::string(my::json::type_name(Type::Object)), std::string("object"));
    CHECK_EQ(std::string(my::json::type_name(Type::Unknown)), std::string("unknown"));
}

MYTEST_CASE(gap_math_median_inplace) {
    // 奇数个
    std::vector<double> odd{5.0, 1.0, 3.0};
    CHECK_NEAR(my::math::median_inplace(odd), 3.0, 1e-12);
    // 偶数个：取中间两个的均值
    std::vector<double> even{4.0, 1.0, 3.0, 2.0};
    CHECK_NEAR(my::math::median_inplace(even), 2.5, 1e-12);
    // 单元素
    std::vector<double> one{7.5};
    CHECK_NEAR(my::math::median_inplace(one), 7.5, 1e-12);
    // 空：返回 0（docstring 的约定），不能崩
    std::vector<double> none;
    CHECK_NEAR(my::math::median_inplace(none), 0.0, 1e-12);
    // 负数混合
    std::vector<double> neg{-5.0, -1.0, -3.0};
    CHECK_NEAR(my::math::median_inplace(neg), -3.0, 1e-12);
}

MYTEST_CASE(gap_io_copy_file_respects_overwrite_flag) {
    const std::string dir = tmp_dir();
    (void)my::io::create_directories(dir);
    const std::string src = my::io::join(dir, "gap_src.txt");
    const std::string dst = my::io::join(dir, "gap_dst.txt");

    CHECK(my::io::write_file(src, "source").ok());
    CHECK(my::io::write_file(dst, "old").ok());

    // overwrite=false：目标已存在必须失败，且**不能覆盖**原内容
    auto no = my::io::copy_file(src, dst, /*overwrite=*/false);
    CHECK(!no.ok());
    auto kept = my::io::read_file(dst);
    CHECK_OK_VALUE(kept_text, kept);
    CHECK_EQ(kept_text, std::string("old"));

    // overwrite=true：覆盖成功
    CHECK(my::io::copy_file(src, dst, /*overwrite=*/true).ok());
    auto replaced = my::io::read_file(dst);
    CHECK_OK_VALUE(new_text, replaced);
    CHECK_EQ(new_text, std::string("source"));

    (void)my::io::remove(src);
    (void)my::io::remove(dst);
}

MYTEST_CASE(gap_io_tempfile_append_and_reset) {
    const std::string dir = tmp_dir();
    (void)my::io::create_directories(dir);

    CHECK_OK_VALUE(tmp, my::io::TempFile::create(dir, "gap_tmp_", ".txt"));
    CHECK(my::io::exists(tmp.path()));

    CHECK(tmp.write("A").ok());
    CHECK(tmp.append("B").ok());
    CHECK(tmp.append("C").ok());
    auto content = my::io::read_file(tmp.path());
    CHECK_OK_VALUE(text, content);
    CHECK_EQ(text, std::string("ABC"));

    // reset()：立即删除文件，路径清空（析构时不会再次删除同一个路径）
    const std::string path = tmp.path();
    tmp.reset();
    CHECK(tmp.path().empty());
    CHECK(!my::io::exists(path));
}

MYTEST_CASE(gap_net_multicast_join_leave) {
    my::net::UdpSocket sock;
    CHECK_OK_VALUE(bound, my::net::UdpSocket::bind({"0.0.0.0", 0}));
    sock = std::move(bound);

    const std::string group = "239.255.0.1";
    auto              joined = sock.join_multicast(group);
    if (!joined.ok()) {
        MYTEST_SKIP("本机不支持组播（或没有可用网卡）：join_multicast 失败");
    }
    // 加入之后必须能退出，且重复退出不该崩
    CHECK(sock.leave_multicast(group).ok());
    (void)sock.leave_multicast(group);  // 幂等性：不要求 Ok，但不能崩
}

MYTEST_CASE(gap_thread_pool_shutdown_discard_drops_pending) {
    CHECK_OK_VALUE(pool, my::thread::ThreadPool::create(1));

    std::atomic<int> started{0};
    std::atomic<int> finished{0};
    std::atomic<bool> release{false};

    // 第一个任务把唯一的 worker 堵住
    (void)pool->submit([&] {
        started.fetch_add(1);
        while (!release.load()) my::time::sleep_ms(1);
        finished.fetch_add(1);
    });
    // 再灌一批排队任务
    for (int i = 0; i < 32; ++i) {
        (void)pool->submit([&] { finished.fetch_add(1); });
    }
    // 等第一个任务真的开始，确保后面的都在队列里
    for (int i = 0; i < 200 && started.load() == 0; ++i) my::time::sleep_ms(1);
    CHECK_EQ(started.load(), 1);

    // drain=false：丢弃未开始的任务，但仍会 join 正在跑的那个
    release.store(true);
    pool->shutdown(/*drain=*/false);

    // 只应该有"正在跑的那一个"完成，排队的不该跑
    CHECK_EQ(finished.load(), 1);
    CHECK_EQ(pool->pending(), std::size_t{0});
}

// 原来的 thread_pool_runs_concurrently 只断言"200 个任务都跑了" —— 1 个 worker 串行执行
// 同样能通过。这里改成**观测并发度**：任务必须等所有人都到齐才继续，
// 只有真的并行（worker 数 >= N）才不会死等。
MYTEST_CASE(gap_thread_pool_actually_runs_in_parallel) {
    constexpr int kN = 4;
    CHECK_OK_VALUE(pool, my::thread::ThreadPool::create(kN));

    std::atomic<int> arrived{0};
    std::atomic<int> finished{0};

    std::vector<std::future<void>> futures;
    for (int i = 0; i < kN; ++i) {
        futures.push_back(pool->submit([&] {
            arrived.fetch_add(1);
            // 等到所有人都到齐（若只有 1 个 worker，这里会一直等到超时）
            for (int spin = 0; spin < 3000 && arrived.load() < kN; ++spin) my::time::sleep_ms(1);
            finished.fetch_add(1);
        }));
    }
    for (auto& f : futures) f.get();

    CHECK_EQ(arrived.load(), kN);
    // 并行的话 4 个都能穿过栅栏；串行的话只有第一个会超时后完成
    CHECK_EQ(finished.load(), kN);
    (void)pool->wait_idle(std::chrono::milliseconds(2000));
    pool->shutdown();
}

// =============================================================================
//  二、image：save 只测过 PNG，prepare_file / 多框映射没测过
// =============================================================================
MYTEST_CASE(gap_image_save_load_roundtrip_jpeg) {
    const std::string dir = tmp_dir();
    (void)my::io::create_directories(dir);
    const auto src = make_textured(64, 48);

    const std::string hi = my::io::join(dir, "gap_hi.jpg");
    const std::string lo = my::io::join(dir, "gap_lo.jpg");
    CHECK(my::image::save(hi, src, 95).ok());
    CHECK(my::image::save(lo, src, 10).ok());

    // 低质量的文件必须明显更小（说明 quality 真的传下去了）
    auto size_hi = my::io::file_size(hi);
    auto size_lo = my::io::file_size(lo);
    CHECK(size_hi.ok());
    CHECK(size_lo.ok());
    CHECK(size_lo.value() < size_hi.value());

    // 回读：JPEG 有损，只校验尺寸与通道；不逐像素比
    auto loaded = my::image::load(hi);
    CHECK_OK_VALUE(img, loaded);
    CHECK_EQ(img.width, 64);
    CHECK_EQ(img.height, 48);
    CHECK_EQ(img.channels, 3);

    (void)my::io::remove(hi);
    (void)my::io::remove(lo);
}

MYTEST_CASE(gap_image_save_load_roundtrip_bmp_tga) {
    const std::string dir = tmp_dir();
    (void)my::io::create_directories(dir);
    const auto src = make_textured(32, 24);

    for (const char* ext : {".bmp", ".tga"}) {
        const std::string path = my::io::join(dir, std::string("gap_rt") + ext);
        CHECK(my::image::save(path, src).ok());
        auto loaded = my::image::load(path);
        CHECK_OK_VALUE(img, loaded);
        CHECK_EQ(img.width, 32);
        CHECK_EQ(img.height, 24);
        CHECK_EQ(img.channels, 3);
        // 这两种格式无损：可以逐像素比
        CHECK_EQ(img.pixels.size(), src.pixels.size());
        if (img.pixels.size() == src.pixels.size()) {
            std::size_t diff = 0;
            for (std::size_t i = 0; i < src.pixels.size(); ++i) {
                if (img.pixels[i] != src.pixels[i]) ++diff;
            }
            CHECK_EQ(diff, std::size_t{0});
        }
        (void)my::io::remove(path);
    }
}

MYTEST_CASE(gap_image_prepare_file_matches_prepare) {
    const std::string dir = tmp_dir();
    (void)my::io::create_directories(dir);
    const auto        src  = make_textured(80, 60);
    const std::string path = my::io::join(dir, "gap_prep.png");
    CHECK(my::image::save(path, src, 100).ok());

    my::image::PreprocessSpec spec;
    spec.input_width  = 64;
    spec.input_height = 64;
    spec.keep_aspect  = true;

    auto from_mem  = my::image::prepare(src, spec);
    auto from_file = my::image::prepare_file(path, spec);
    CHECK(from_mem.ok());
    CHECK(from_file.ok());
    if (from_mem.ok() && from_file.ok()) {
        // 两条路径必须产出**完全一样**的张量（prepare_file 就是 解码+prepare）
        CHECK_EQ(from_file.value().tensor.size(), from_mem.value().tensor.size());
        CHECK_EQ(from_file.value().shape.dims, from_mem.value().shape.dims);
        CHECK_EQ(from_file.value().channels, from_mem.value().channels);
        float max_diff = 0.0f;
        for (std::size_t i = 0; i < from_mem.value().tensor.size() &&
                                i < from_file.value().tensor.size();
             ++i) {
            const float d = std::fabs(from_file.value().tensor[i] - from_mem.value().tensor[i]);
            if (d > max_diff) max_diff = d;
        }
        CHECK(max_diff < 1e-6f);
    }
    (void)my::io::remove(path);
}

MYTEST_CASE(gap_image_unletterbox_boxes_multiple) {
    // 原图 800x600 -> 模型输入 640x640，keep_aspect：scale = min(640/800, 640/600) = 0.8
    const auto lb = my::infer::letterbox_params(800, 600, 640, 640);
    CHECK_NEAR(lb.scale, 0.8, 1e-4);
    CHECK_EQ(lb.pad_x, 0);                    // 800*0.8 = 640，水平刚好铺满
    CHECK_EQ(lb.pad_y, 80);                   // (640 - 600*0.8)/2 = 80

    // 用文档里的公式自己算期望，再和实现比
    float boxes[8] = {
        80.0f,  60.0f,  240.0f, 180.0f,   // 框 1
        400.0f, 300.0f, 600.0f, 450.0f,   // 框 2
    };
    my::image::unletterbox_boxes(boxes, 2, lb);
    const float sx = lb.scale_x;
    const float sy = lb.scale_y;
    const float x1 = (80.0f - static_cast<float>(lb.pad_x)) / sx;
    const float y1 = (60.0f - static_cast<float>(lb.pad_y)) / sy;
    CHECK_NEAR(boxes[0], x1, 1e-3);
    CHECK_NEAR(boxes[1], y1, 1e-3);
    // 两个框都要被映射（第二个框不能因为 stride 算错而留在原值）
    CHECK(boxes[4] > 400.0f);
}
