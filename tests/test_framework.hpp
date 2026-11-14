#pragma once
// =============================================================================
//  极简测试框架（无第三方依赖，交叉编译也能跑）
//
//  MYTEST_CASE(name) { ... }       定义一个用例
//  CHECK(cond)                     断言；失败即结束该用例
//  CHECK_EQ / CHECK_NEAR / CHECK_OK / CHECK_ERR
// =============================================================================

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace mytest {

struct Case {
    const char* name;
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

inline void register_case(const char* name, void (*fn)()) { cases().push_back(Case{name, fn}); }

inline void fail(const char* expr, const char* file, int line) {
    ++failures();
    std::printf("  [FAIL] %s\n         at %s:%d\n         expr: %s\n", current_case(), file, line,
                expr);
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { register_case(name, fn); }
};

inline int run_all() {
    int failed_cases = 0;
    std::printf("运行 %zu 个用例\n", cases().size());
    for (auto& c : cases()) {
        current_case() = c.name;
        const int before = failures();
        std::printf("[ RUN  ] %s\n", c.name);
        c.fn();
        if (failures() == before) {
            std::printf("[  OK  ] %s\n", c.name);
        } else {
            ++failed_cases;
        }
    }
    std::printf("\n结果: %zu 个用例, %d 个失败断言, %d 个失败用例\n", cases().size(), failures(),
                failed_cases);
    return failed_cases == 0 ? 0 : 1;
}

}  // namespace mytest

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
