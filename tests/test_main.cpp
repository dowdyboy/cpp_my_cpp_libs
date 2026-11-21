#include "test_framework.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// 支持以下几种过滤方式，ctest 会据此按用例逐个注册：
//   mylib_tests --case=preprocess_nms       只跑这一个用例（名字必须完全一致）
//   mylib_tests --suite=preprocess          跑一个套件
//   mylib_tests --filter=nms                按用例名子串
//   mylib_tests --list                      列出所有用例（suite:name）
//   mylib_tests --expect-cases=356          用例总数必须正好是 356（守卫"用例静默消失"）
//   mylib_tests --fail-on-skip              有 skip 的用例就判失败
int main(int argc, char** argv) {
    mytest::RunOptions opt;
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
                        "          [--expect-cases=N] [--fail-on-skip]\n",
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
        if (!list) return 0;  // 只校验数量，不跑用例
    }

    if (list) {
        mytest::list_cases();
        return 0;
    }
    return mytest::run(opt);
}
