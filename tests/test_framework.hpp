#pragma once
// =============================================================================
//  极简测试框架（无第三方依赖，交叉编译也能跑）
//
//  MYTEST_SUITE("core")            声明本文件属于哪个套件（每个 test_*.cpp 一行）
//  MYTEST_CASE(name) { ... }       定义一个用例（名字全局唯一，套件内自动带前缀）
//  CHECK(cond)                     断言；失败即结束该用例
//  CHECK_EQ / CHECK_NEAR / CHECK_OK / CHECK_ERR / CHECK_RESULT / CHECK_OK_VALUE
//
//  命令行：
//      mylib_tests                      跑全部
//      mylib_tests --suite=preprocess   只跑某个套件（ctest 按套件注册）
//      mylib_tests --filter=nms         按用例名子串过滤
//      mylib_tests --list               列出所有用例（suite:name）
//
//  为什么要套件：一个用例失败时原来的实现只能靠"记一条 FAIL 然后 return"，
//  但 `Result::value()` 在失败时会 abort 整个进程 —— 套件拆分后，
//  即使某个套件把进程带走，其余套件仍然能给出结论。
// =============================================================================

#include <cmath>
#include <cstdio>
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

struct SuiteRegistrar {
    explicit SuiteRegistrar(const char* suite) { current_suite() = suite; }
};

struct Registrar {
    Registrar(const char* name, void (*fn)()) { register_case(name, fn); }
};

inline void list_cases() {
    for (const auto& c : cases()) std::printf("%s:%s\n", c.suite, c.name);
}

inline int run(const char* name_filter, const char* suite_filter, bool exact_name = false) {
    int failed_cases = 0;
    std::size_t ran = 0;
    std::size_t skipped = 0;
    std::size_t matched = 0;

    for (auto& c : cases()) {
        if (suite_filter != nullptr && suite_filter[0] != '\0' &&
            std::strcmp(c.suite, suite_filter) != 0) {
            ++skipped;
            continue;
        }
        if (name_filter != nullptr && name_filter[0] != '\0') {
            const bool hit = exact_name ? (std::strcmp(c.name, name_filter) == 0)
                                        : (std::strstr(c.name, name_filter) != nullptr);
            if (!hit) {
                ++skipped;
                continue;
            }
        }
        ++matched;

        current_case() = c.name;
        const int before = failures();
        ++ran;
        std::printf("[ RUN  ] %s\n", c.name);
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
        if (failures() == before) {
            std::printf("[  OK  ] %s\n", c.name);
        } else {
            ++failed_cases;
        }
    }

    // 精确指名却一个都没匹配上：说明 ctest 注册的用例名和源码脱钩了（改名/删用例后没重新 configure），
    // 这种情况必须报错，否则测试会"静默通过"。
    if (exact_name && name_filter != nullptr && matched == 0) {
        std::printf("错误: 找不到名为 \"%s\" 的用例（ctest 注册与源码不一致，请重新 configure）\n",
                    name_filter);
        return 2;
    }

    std::printf("\n结果: %zu 个用例, %d 个失败断言, %d 个失败用例", ran, failures(), failed_cases);
    if (skipped > 0) std::printf("（跳过 %zu 个）", skipped);
    std::printf("\n");
    return failed_cases == 0 ? 0 : 1;
}

inline int run_all() { return run(nullptr, nullptr); }

}  // namespace mytest

#define MYTEST_SUITE(tag) static const ::mytest::SuiteRegistrar _mytest_suite_registrar(tag)

#define MYTEST_CASE(name)                                                    \
    static void name();                                                      \
    static ::mytest::Registrar name##_registrar(#name, &name);               \
    static void name()

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
        if (std::fabs(_va - _vb) > (eps)) {                                  \
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
