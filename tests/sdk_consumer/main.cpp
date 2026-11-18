// =============================================================================
//  SDK 消费方验证程序
//
//  只使用安装树里的公共头 + find_package 提供的 target，完全不出现
//  spdlog / nlohmann / Eigen / ONNX Runtime。任何一步失败都返回非零退出码，
//  这样 CI 能把它当作真实回归而不是"能编过就行"。
// =============================================================================
#include <my_cpp_libs/core/build_info.hpp>
#include <my_cpp_libs/core/status.hpp>
#include <my_cpp_libs/dsp/dsp.hpp>
#include <my_cpp_libs/io/io.hpp>
#include <my_cpp_libs/json/config.hpp>
#include <my_cpp_libs/json/json.hpp>
#include <my_cpp_libs/log/log.hpp>
#include <my_cpp_libs/net/net.hpp>
#include <my_cpp_libs/thread/thread.hpp>
#include <my_cpp_libs/time/time.hpp>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool cond, const char* what) {
    if (!cond) {
        std::printf("  [FAIL] %s\n", what);
        ++failures;
    }
}

}  // namespace

int main() {
    // ---- core / BuildInfo ----
    const char* summary = my::BuildInfo::summary();
    expect(summary != nullptr && summary[0] != '\0', "BuildInfo::summary() 非空");
    std::printf("build: %s\n", summary);

    // ---- log ----
    my::log::Config cfg;
    cfg.level      = my::log::Level::Info;
    cfg.to_console = true;
    expect(static_cast<bool>(my::log::init(cfg)), "log::init 成功");

    // ---- json ----
    auto doc = my::json::Document::parse(R"({"server":{"port":8080},"list":[1,2,3]})");
    expect(static_cast<bool>(doc), "JSON 解析成功");
    if (doc) {
        expect(doc.value().get_or<int>("server.port", -1) == 8080, "路径读取 server.port");
        expect(doc.value().get_or<int>("list[2]", -1) == 3, "数组下标读取 list[2]");
        // 越界下标必须被拒（不是 OOM、不是静默补 null）
        expect(!doc.value().set("huge[99999999]", 1), "超大数组下标被拒绝");
    }

    // ---- dsp ----
    const double       fs = 8000.0;
    std::vector<float> x(1024);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979 * 1000.0 *
                                           static_cast<double>(i) / fs));
    }
    auto spec = my::dsp::welch(x, fs, 1024);
    expect(static_cast<bool>(spec), "welch 成功");
    if (spec) {
        expect(std::fabs(spec.value().peak_freq - 1000.0) < 1.0, "谱峰在 1 kHz");
    }

    // ---- net ----
    auto ep = my::net::parse_endpoint("127.0.0.1:9000");
    expect(static_cast<bool>(ep), "endpoint 解析成功");
    if (ep) {
        expect(ep.value().host == "127.0.0.1", "endpoint host");
        expect(ep.value().port == 9000, "endpoint port");
    }
    // 只给主机名、不给端口：按默认端口 0 成功解析（port 由调用方决定）
    auto no_port = my::net::parse_endpoint("no-port-at-all");
    expect(static_cast<bool>(no_port) && no_port.value().port == 0,
           "缺少端口时按 0 处理");
    expect(!ep.value().empty(), "endpoint 非空");

    my::log::shutdown();

    // ---- io / time / thread（0.1.2 新增组件的 SDK 可见性）----
    const std::string tmp_file = "sdk_consumer_roundtrip.txt";
    expect(static_cast<bool>(my::io::write_file(tmp_file, "sdk")), "io::write_file 成功");
    auto back = my::io::read_file(tmp_file);
    expect(static_cast<bool>(back) && back.value() == "sdk", "io::read_file 读回一致");
    expect(static_cast<bool>(my::io::remove(tmp_file)), "io::remove 成功");

    my::time::Stopwatch watch;
    my::time::sleep_ms(5);
    expect(watch.elapsed_ms() >= 2.0, "Stopwatch 测到耗时");

    auto pool = my::thread::ThreadPool::create(2);
    expect(static_cast<bool>(pool), "ThreadPool 创建成功");
    if (pool) {
        std::atomic<int> hits{0};
        for (int i = 0; i < 8; ++i) {
            (void)pool.value()->submit([&hits] { hits.fetch_add(1); });
        }
        expect(pool.value()->wait_idle(std::chrono::milliseconds(5000)), "线程池任务完成");
        expect(hits.load() == 8, "线程池执行了全部任务");
        pool.value()->shutdown();
    }

    if (failures != 0) {
        std::printf("SDK 消费方验证失败: %d 项\n", failures);
        return 1;
    }
    std::printf("SDK 消费方验证通过\n");
    return 0;
}
