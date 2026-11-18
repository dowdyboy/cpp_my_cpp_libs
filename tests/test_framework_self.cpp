// =============================================================================
//  测试框架自测
//
//  由来：框架本身出问题会**让所有用例一起说谎**，所以它必须自己有测试。
//  这里的重点是 mytest::near() —— 全仓浮点断言都走它，之前
//  `fabs(a-b) > eps` 的写法对 NaN 恒为假，导致 NaN 一路"通过"。
// =============================================================================
#include "test_framework.hpp"

#include <cmath>
#include <limits>

MYTEST_SUITE("framework");

MYTEST_CASE(framework_near_rejects_nan) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    // NaN 与任何值（包括它自己）都不"近似相等"
    CHECK(!mytest::near(nan, nan, 1e-6));
    CHECK(!mytest::near(nan, 1.0, 1e-6));
    CHECK(!mytest::near(1.0, nan, 1e-6));
    CHECK(!mytest::near(nan, 0.0, std::numeric_limits<double>::infinity()));
}

MYTEST_CASE(framework_near_handles_inf) {
    const double inf = std::numeric_limits<double>::infinity();
    CHECK(mytest::near(inf, inf, 1e-6));    // 同号无穷：视为相等
    CHECK(!mytest::near(inf, 1.0, 1e-6));   // 无穷 vs 有限：不相等
    CHECK(!mytest::near(1.0, inf, 1e-6));
    CHECK(!mytest::near(-inf, inf, 1e-6));
}

MYTEST_CASE(framework_near_tolerance_boundary) {
    CHECK(mytest::near(1.0, 1.0, 1e-9));
    CHECK(mytest::near(1.0, 1.0 + 1e-7, 1e-6));
    CHECK(!mytest::near(1.0, 1.0 + 1e-5, 1e-6));
    // 负 eps 与正 eps 等价（取绝对值），不能因此把所有比较都判失败
    CHECK(mytest::near(1.0, 1.0 + 1e-7, -1e-6));
    // 0 容差：只有精确相等才过
    CHECK(mytest::near(2.5, 2.5, 0.0));
    CHECK(!mytest::near(2.5, 2.5000001, 0.0));
}

MYTEST_CASE(framework_registry_is_populated) {
    // 用例注册表非空，且每一项都带名字与套件名（忘了 MYTEST_SUITE 会是 "default"）
    CHECK(!mytest::cases().empty());
    std::size_t framework_cases = 0;
    for (const auto& c : mytest::cases()) {
        CHECK(c.name != nullptr);
        CHECK(c.suite != nullptr);
        CHECK(c.fn != nullptr);
        if (std::string(c.suite) == "framework") ++framework_cases;
    }
    CHECK(framework_cases >= 4);
}
