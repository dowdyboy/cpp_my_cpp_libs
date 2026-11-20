// =============================================================================
//  "只链单个组件"的消费方 —— 专门用来抓 SDK 包的链接缺陷
//
//  为什么需要它：`sdk_consumer` 链的是 `my_cpp_libs::all`，而 ::all 里显式列了 core，
//  所以下面两类问题它永远抓不到：
//    * 组件在 SDK 的 Config 里没带 `my_cpp_libs::core`（0.1.7 及以前：单链 ::http 必挂，
//      因为 http 自己会调 Status::to_string）；
//    * 组件的实现里有某个三方后端的符号引用，但包里没带那个库
//      （0.1.7 及以前：包里的 infer 引用了 ncnn，包里却没有 ncnn）。
//
//  所以这里**故意**不链 ::all、也不链 core，只链 http / infer / serial，
//  然后各调一个"必然用到该组件自身与 core 符号"的函数。
// =============================================================================

#include <cstdio>

#include <my_cpp_libs/http/http.hpp>
#include <my_cpp_libs/infer/infer.hpp>
#include <my_cpp_libs/serial/serial.hpp>

int main() {
    int failures = 0;

    // core 的符号：任何组件都会用到 Status。这里同时也验证了"组件把 core 带进来"。
    const my::Status probe(my::Errc::IoError, "probe");
    if (probe.to_string().empty()) {
        std::printf("  [FAIL] Status::to_string 返回空\n");
        ++failures;
    }

    // http 自身的符号（Client 的构造/析构在 libmy_cpp_libs_http 里）
    my::http::Client client;
    (void)client;

    // serial 自身的符号
    my::serial::Config scfg;
    if (!my::serial::validate(scfg).ok()) {
        std::printf("  [FAIL] serial 默认配置校验失败\n");
        ++failures;
    }

    // infer 自身的符号：available_backends / build_backend 都在 libmy_cpp_libs_infer 里。
    // 这里不加载模型（包里的模型路径不一定存在），只验证符号可解析。
    const auto backends = my::infer::available_backends();
    const char* backend = my::infer::build_backend();
    if (backend == nullptr) {
        std::printf("  [FAIL] infer::build_backend 返回空\n");
        ++failures;
    }
    std::printf("  infer 后端: %s（可用 %zu 个）\n", backend ? backend : "(null)",
                backends.size());

    if (failures != 0) {
        std::printf("单组件链接验证失败: %d 项\n", failures);
        return 1;
    }
    std::printf("单组件链接验证通过（http / infer / serial 各自独立可链）\n");
    return 0;
}
