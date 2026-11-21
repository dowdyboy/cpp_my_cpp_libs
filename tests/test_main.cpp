#include "test_framework.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// 支持以下几种过滤方式，ctest 会据此按用例逐个注册：
//   mylib_tests --case=preprocess_nms       只跑这一个用例（名字必须完全一致）
//   mylib_tests --suite=preprocess          跑一个套件
//   mylib_tests --filter=nms                按用例名子串
//   mylib_tests --list                      列出所有用例（suite:name）
//   mylib_tests --expect-cases=N            用例总数必须正好是 N（守卫"用例静默消失"）
//   mylib_tests --min-suite=net:30          指定套件至少 N 条（守卫"整个 TU 掉出列表"，
//                                           可重复；由 tests/CMakeLists.txt 按当前值传入）
//   mylib_tests --fail-on-skip              有 skip 的用例就判失败
int main(int argc, char** argv) {
    mytest::RunOptions opt;
    std::vector<mytest::SuiteRequirement> min_suites;
    bool list          = false;
    long expected      = -1;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strncmp(a, "--suite=", 8) == 0) {
            opt.suite_filter = a + 8;
        } else if (std::strncmp(a, "--case=", 7) == 0) {
            opt.name_filter = a + 7;
            opt.exact_name  = true;
        } else if (std::strncmp(a, "--filter=", 9) == 0) {
            opt.name_filter = a + 9;
            opt.exact_name  = false;
        } else if (std::strncmp(a, "--expect-cases=", 15) == 0) {
            expected = std::strtol(a + 15, nullptr, 10);
        } else if (std::strncmp(a, "--min-suite=", 12) == 0) {
            const char* spec  = a + 12;
            const char* colon = std::strrchr(spec, ':');
            if (colon == nullptr || colon == spec || colon[1] == '\0') {
                std::printf("错误: --min-suite 的格式应为 name:N（收到 %s）\n", spec);
                return 2;
            }
            mytest::SuiteRequirement req;
            req.name    = std::string(spec, static_cast<std::size_t>(colon - spec));
            req.minimum = static_cast<int>(std::strtol(colon + 1, nullptr, 10));
            if (req.minimum <= 0) {
                std::printf("错误: --min-suite 的 N 必须 > 0（收到 %s）\n", spec);
                return 2;
            }
            min_suites.push_back(std::move(req));
        } else if (std::strcmp(a, "--fail-on-skip") == 0) {
            opt.fail_on_skip = true;
        } else if (std::strcmp(a, "--list") == 0) {
            list = true;
        } else if (a[0] != '-') {
            opt.name_filter = a;  // 位置参数当作用例名子串
            opt.exact_name  = false;
        } else {
            std::printf("未知参数: %s\n"
                        "用法: %s [--case=NAME] [--suite=NAME] [--filter=SUBSTR] [--list]\n"
                        "          [--expect-cases=N] [--min-suite=NAME:N]... [--fail-on-skip]\n",
                        a, argv[0]);
            return 2;
        }
    }

    // 用例数守卫：CMake 侧按源码静态扫描得到 N 并注册 N 个 ctest 用例；
    // 二进制里实际注册的用例数必须与之一致。两者不一致说明扫描漏了/多了某个 TU
    // （例如新增 test_*.cpp 却没加进 _test_sources），那种"少跑几个用例还全绿"
    // 是最难发现的假阳性。
    if (expected >= 0) {
        const long actual = static_cast<long>(mytest::cases().size());
        if (actual != expected) {
            std::printf("错误: 期望 %ld 个用例，实际注册 %ld 个 —— "
                        "CTest 注册表与源码不一致（检查 tests/CMakeLists.txt 的用例扫描）\n",
                        expected, actual);
            return 2;
        }
        std::printf("用例数校验通过: %ld\n", actual);
    }

    // 套件最小条数：expect-cases 只比较"二进制 vs 参与编译的源码"，整套 TU
    // 掉出 _test_sources 时两边一起缩水；这里按套件兜底。
    if (!min_suites.empty() && !mytest::check_suite_minima(min_suites)) return 2;
    if (expected >= 0 && !list) return 0;  // 只校验数量，不跑用例

    if (list) {
        mytest::list_cases();
        return 0;
    }
    return mytest::run(opt);
}
