#pragma once
// =============================================================================
//  极简测试框架（无第三方依赖，交叉编译也能跑）
//
//  MYTEST_SUITE("core")            声明本文件属于哪个套件（每个 test_*.cpp 一行）
//  MYTEST_CASE(name) { ... }       定义一个用例（名字全局唯一，套件内自动带前缀）
//  CHECK(cond)                     断言；失败即结束该用例
//  CHECK_EQ / CHECK_NEAR / CHECK_OK / CHECK_ERR / CHECK_RESULT / CHECK_OK_VALUE
//  EXPECT_*                        同样的断言，但**失败后继续执行**（想看全部问题用）
//  MYTEST_SKIP("原因")             本用例在当前构建下无法执行（如没有 ORT 后端）
//
//  命令行：
//      mylib_tests                       跑全部
//      mylib_tests --suite=preprocess    只跑某个套件（ctest 按用例注册）
//      mylib_tests --filter=nms          按用例名子串过滤
//      mylib_tests --case=nms_basic      精确跑单个用例（名字必须完全一致）
//      mylib_tests --list                列出所有用例（suite:name）
//      mylib_tests --expect-cases=158    注册的用例数必须正好是 158，否则退出码 2
//      mylib_tests --fail-on-skip        有被 skip 的用例就判失败
//
//  为什么要套件：一个用例失败时原来的实现只能靠"记一条 FAIL 然后 return"，
//  但 `Result::value()` 在失败时会 abort 整个进程 —— 套件拆分后，
//  即使某个套件把进程带走，其余套件仍然能给出结论。
//
//  为什么还要装信号处理器：套件粒度的隔离在 `ctest -j` 下够用（每个用例一个进程），
//  但直接跑 `mylib_tests` 时一次崩溃仍然会带走整个进程、什么结论都没有。
//  装了处理器之后至少能打印"是哪个用例崩的"，ctest 那边也能拿到非零退出码。
// =============================================================================

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace mytest {

struct Case {
    const char* name;
    const char* suite;
    void (*fn)();
};

inline std::vector<Case>& cases() {
    static std::vector<Case> v;
    return v;
}

inline int& failures() {
    static int n = 0;
    return n;
}

inline int& skips() {
    static int n = 0;
    return n;
}

inline const char*& current_case() {
    static const char* s = "";
    return s;
}

/// 当前翻译单元的套件名（由 MYTEST_SUITE 设置，静态初始化阶段生效）
inline const char*& current_suite() {
    static const char* s = "default";
    return s;
}

inline void register_case(const char* name, void (*fn)()) {
    cases().push_back(Case{name, current_suite(), fn});
}

inline void fail(const char* expr, const char* file, int line) {
    ++failures();
    std::printf("  [FAIL] %s\n         at %s:%d\n         expr: %s\n", current_case(), file, line,
                expr);
}

inline void skip(const char* reason) {
    ++skips();
    std::printf("  [SKIP] %s\n         %s\n", current_case(), reason ? reason : "");
}

/// 浮点近似比较（CHECK_NEAR / EXPECT_NEAR 的判定核心，单独抽出来是为了能自测）。
///
/// 关键：`fabs(a-b) > eps` 这种写法对 NaN **恒为假**，于是 NaN 永远"通过"。
/// 全仓 180 处浮点断言都靠它，所以这里必须显式判 NaN。
inline bool near(double a, double b, double eps) {
    if (a == b) return true;  // 精确相等，顺带处理 ±inf 同号
    if (std::isnan(a) || std::isnan(b)) return false;
    if (std::isinf(a) || std::isinf(b)) return false;
    return std::fabs(a - b) <= std::fabs(eps);
}

/// 信号处理器里只做 async-signal-safe 的事（printf/fflush/_Exit）
inline void crash_handler(int sig) {
    const char* name = current_case();
    std::fprintf(stderr,
                 "\n[CRASH] 用例 '%s' 被信号 %d 终止（段错误/断言失败/abort 等）\n"
                 "        提示：用 `--case=%s` 单独跑，或看 ctest 的 --output-on-failure\n",
                 name, sig, name);
    std::fflush(stderr);
    std::_Exit(3);
}

inline void install_crash_handlers() {
    static bool done = false;
    if (done) return;
    done = true;
    std::signal(SIGSEGV, crash_handler);
    std::signal(SIGABRT, crash_handler);
    std::signal(SIGFPE, crash_handler);
    std::signal(SIGILL, crash_handler);
#ifdef SIGBUS
    std::signal(SIGBUS, crash_handler);
#endif
}

struct SuiteRegistrar {
    explicit SuiteRegistrar(const char* suite) { current_suite() = suite; }
};

struct Registrar {
    Registrar(const char* name, void (*fn)()) { register_case(name, fn); }
};

inline void list_cases() {
    for (const auto& c : cases()) std::printf("%s:%s\n", c.suite, c.name);
}

struct RunOptions {
    const char* name_filter  = nullptr;
    const char* suite_filter = nullptr;
    bool        exact_name   = false;
    bool        fail_on_skip = false;
};

inline int run(const RunOptions& opt) {
    install_crash_handlers();

    int         failed_cases  = 0;
    int         skipped_cases = 0;
    std::size_t ran           = 0;
    std::size_t not_selected  = 0;
    std::size_t matched       = 0;

    for (auto& c : cases()) {
        if (opt.suite_filter != nullptr && opt.suite_filter[0] != '\0' &&
            std::strcmp(c.suite, opt.suite_filter) != 0) {
            ++not_selected;
            continue;
        }
        if (opt.name_filter != nullptr && opt.name_filter[0] != '\0') {
            const bool hit = opt.exact_name
                                 ? (std::strcmp(c.name, opt.name_filter) == 0)
                                 : (std::strstr(c.name, opt.name_filter) != nullptr);
            if (!hit) {
                ++not_selected;
                continue;
            }
        }
        ++matched;

        current_case() = c.name;
        const int before_fail = failures();
        const int before_skip = skips();
        ++ran;
        std::printf("[ RUN  ] %s\n", c.name);
        // 立刻 flush：stdout 重定向到文件时是块缓冲，用例一旦挂住/被 kill，
        // 缓冲区里的内容会丢，日志里就只剩"上一条"——根本看不出是哪个用例卡的。
        std::fflush(stdout);
        try {
            c.fn();
        } catch (const std::exception& e) {
            // 用例抛异常不能让后面的用例全丢：记一条失败继续
            std::printf("  [FAIL] %s\n         at %s (抛异常)\n         what: %s\n", c.name,
                        c.suite, e.what());
            ++failures();
        } catch (...) {
            std::printf("  [FAIL] %s\n         at %s (抛出未知异常)\n", c.name, c.suite);
            ++failures();
        }

        if (failures() != before_fail) {
            ++failed_cases;
        } else if (skips() != before_skip) {
            ++skipped_cases;  // MYTEST_SKIP 已经打过 [SKIP] 了
        } else {
            std::printf("[  OK  ] %s\n", c.name);
        }
    }

    // 明确指定了过滤条件却一个都没匹配上：说明 ctest 注册的用例名和源码脱钩了
    // （改名/删用例后没重新 configure），必须报错，否则测试会"静默通过"。
    const bool has_filter = (opt.exact_name && opt.name_filter != nullptr &&
                             opt.name_filter[0] != '\0') ||
                            (opt.suite_filter != nullptr && opt.suite_filter[0] != '\0') ||
                            (opt.name_filter != nullptr && opt.name_filter[0] != '\0');
    if (has_filter && matched == 0) {
        std::printf("错误: 没有任何用例匹配当前过滤条件（ctest 注册与源码不一致，请重新 configure）\n");
        return 2;
    }

    std::printf("\n结果: %zu 个用例, %d 个失败断言, %d 个失败用例", ran, failures(),
                failed_cases);
    if (skipped_cases > 0) std::printf(", %d 个跳过", skipped_cases);
    if (not_selected > 0) std::printf("（未选中 %zu 个）", not_selected);
    std::printf("\n");

    if (failed_cases != 0) return 1;
    if (opt.fail_on_skip && skipped_cases > 0) {
        std::printf("错误: --fail-on-skip 生效，有 %d 个用例被跳过\n", skipped_cases);
        return 1;
    }
    return 0;
}

inline int run(const char* name_filter, const char* suite_filter, bool exact_name = false) {
    RunOptions opt;
    opt.name_filter  = name_filter;
    opt.suite_filter = suite_filter;
    opt.exact_name   = exact_name;
    return run(opt);
}

inline int run_all() { return run(nullptr, nullptr); }

}  // namespace mytest

#define MYTEST_SUITE(tag) static const ::mytest::SuiteRegistrar _mytest_suite_registrar(tag)

#define MYTEST_CASE(name)                                                    \
    static void name();                                                      \
    static ::mytest::Registrar name##_registrar(#name, &name);               \
    static void name()

/// 当前用例在此构建下无法验证（例如没有 ONNX Runtime 后端时 ml 的 golden 对拍）。
/// 记成"跳过"而不是"通过"：汇总里能看见，CI 可以加 --fail-on-skip 把它变成失败。
#define MYTEST_SKIP(reason)                                                  \
    do {                                                                     \
        ::mytest::skip(reason);                                              \
        return;                                                              \
    } while (0)

// ----------------------------------------------------------------------------- 致命断言
// 失败即 return（同一个用例后面的代码不再执行）

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ::mytest::fail(#cond, __FILE__, __LINE__);                       \
            return;                                                          \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

#define CHECK_NEAR(a, b, eps)                                                \
    do {                                                                     \
        const double _va = static_cast<double>(a);                           \
        const double _vb = static_cast<double>(b);                           \
        if (!::mytest::near(_va, _vb, static_cast<double>(eps))) {           \
            std::printf("         实际: %.17g vs 期望: %.17g (eps=%g)\n", _va, _vb, \
                        static_cast<double>(eps));                           \
            ::mytest::fail(#a " ~= " #b, __FILE__, __LINE__);                \
            return;                                                          \
        }                                                                    \
    } while (0)

#define CHECK_OK(expr)                                                       \
    do {                                                                     \
        auto _st = (expr);                                                   \
        if (!_st) {                                                          \
            std::printf("         实际错误: %s\n", _st.to_string().c_str()); \
            ::mytest::fail(#expr " 应当成功", __FILE__, __LINE__);          \
            return;                                                          \
        }                                                                    \
    } while (0)

#define CHECK_ERR(expr)                                                      \
    do {                                                                     \
        auto _st = (expr);                                                   \
        if (_st) {                                                           \
            ::mytest::fail(#expr " 应当失败", __FILE__, __LINE__);          \
            return;                                                          \
        }                                                                    \
    } while (0)

// ----------------------------------------------------------------------------- 非致命断言
// 失败后继续执行，用来一次看全一组独立问题（不改变用例是否失败）

#define EXPECT(cond)                                                         \
    do {                                                                     \
        if (!(cond)) ::mytest::fail(#cond, __FILE__, __LINE__);              \
    } while (0)

#define EXPECT_EQ(a, b) EXPECT((a) == (b))

#define EXPECT_NEAR(a, b, eps)                                               \
    do {                                                                     \
        const double _va = static_cast<double>(a);                           \
        const double _vb = static_cast<double>(b);                           \
        if (!::mytest::near(_va, _vb, static_cast<double>(eps))) {           \
            std::printf("         实际: %.17g vs 期望: %.17g (eps=%g)\n", _va, _vb, \
                        static_cast<double>(eps));                           \
            ::mytest::fail(#a " ~= " #b, __FILE__, __LINE__);                \
        }                                                                    \
    } while (0)

#define EXPECT_TRUE(cond) EXPECT(cond)
#define EXPECT_FALSE(cond) EXPECT(!(cond))

#define MYTEST_DETAIL_CAT_(a, b) a##b
#define MYTEST_DETAIL_CAT(a, b) MYTEST_DETAIL_CAT_(a, b)

#define CHECK_RESULT(expr)                                                   \
    auto&& MYTEST_DETAIL_CAT(_res_, __LINE__) = (expr);                      \
    if (!MYTEST_DETAIL_CAT(_res_, __LINE__)) {                               \
        std::printf("         实际错误: %s\n",                               \
                    MYTEST_DETAIL_CAT(_res_, __LINE__).status().to_string().c_str()); \
        ::mytest::fail(#expr " 应当返回结果", __FILE__, __LINE__);          \
        return;                                                              \
    }

/// 取 Result<T> 的值并绑到一个名字上；失败时记 FAIL 而不是 abort 掉整个进程。
///   CHECK_OK_VALUE(value, doc.get<int>("a.b"));
///   CHECK_EQ(value, 42);
#define CHECK_OK_VALUE(name, expr)                                           \
    auto MYTEST_DETAIL_CAT(_rv_, __LINE__) = (expr);                         \
    if (!MYTEST_DETAIL_CAT(_rv_, __LINE__)) {                                \
        std::printf("         实际错误: %s\n",                               \
                    MYTEST_DETAIL_CAT(_rv_, __LINE__).status().to_string().c_str()); \
        ::mytest::fail(#expr " 应当返回结果", __FILE__, __LINE__);          \
        return;                                                              \
    }                                                                        \
    const auto& name = MYTEST_DETAIL_CAT(_rv_, __LINE__).value()
