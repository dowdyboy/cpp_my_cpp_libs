#include "test_framework.hpp"

#include <my_cpp_libs/math/math.hpp>

#include <cmath>
#include <limits>
#include <random>
#include <vector>

namespace m = my::math;

MYTEST_SUITE("math");

MYTEST_CASE(math_scalar_helpers) {
    CHECK_NEAR(m::deg2rad(180.0), m::kPi, 1e-12);
    CHECK_NEAR(m::rad2deg(m::kPi), 180.0, 1e-12);
    CHECK_NEAR(m::clamp(5.0, 0.0, 3.0), 3.0, 1e-12);
    CHECK_NEAR(m::clamp(-5.0, 0.0, 3.0), 0.0, 1e-12);
    CHECK_NEAR(m::lerp(0.0, 10.0, 0.25), 2.5, 1e-12);
}

MYTEST_CASE(math_euler_roundtrip) {
    std::mt19937                       rng(1234);
    std::uniform_real_distribution<double> roll_dist(-m::kPi, m::kPi);
    std::uniform_real_distribution<double> pitch_dist(-m::kPi / 2 + 0.01, m::kPi / 2 - 0.01);
    std::uniform_real_distribution<double> yaw_dist(-m::kPi, m::kPi);

    for (int i = 0; i < 200; ++i) {
        const double r = roll_dist(rng);
        const double p = pitch_dist(rng);
        const double y = yaw_dist(rng);

        const m::Mat3d R      = m::rotation_zyx(r, p, y);
        const m::Vec3d euler  = m::euler_zyx(R);

        CHECK_NEAR(euler.x(), r, 1e-9);
        CHECK_NEAR(euler.y(), p, 1e-9);
        CHECK_NEAR(euler.z(), y, 1e-9);

        // 旋转矩阵必须是正交阵且 det=+1
        const m::Mat3d ortho_check = R.transpose() * R;
        CHECK_NEAR((ortho_check - m::Mat3d::Identity()).norm(), 0.0, 1e-12);
        CHECK_NEAR(R.determinant(), 1.0, 1e-12);

        // 四元数路径
        const m::Quatd q  = m::quaternion_zyx(r, p, y);
        const m::Vec3d e2 = m::euler_zyx(q);
        CHECK_NEAR(e2.x(), r, 1e-9);
        CHECK_NEAR(e2.y(), p, 1e-9);
        CHECK_NEAR(e2.z(), y, 1e-9);
    }
}

MYTEST_CASE(math_euler_gimbal_lock) {
    // pitch = +90 度时 roll/yaw 退化，必须仍然可解且不产生 NaN
    const m::Mat3d R = m::rotation_zyx(0.3, m::kPi / 2, 0.5);
    const m::Vec3d e = m::euler_zyx(R);

    CHECK(std::isfinite(e.x()));
    CHECK(std::isfinite(e.y()));
    CHECK(std::isfinite(e.z()));
    CHECK_NEAR(e.y(), m::kPi / 2, 1e-6);
    CHECK_NEAR(e.z(), 0.0, 1e-12);

    // 重建的矩阵应与原矩阵一致
    const m::Mat3d R2 = m::rotation_zyx(e.x(), e.y(), e.z());
    CHECK_NEAR((R2 - R).norm(), 0.0, 1e-9);
}

MYTEST_CASE(math_transform) {
    const m::Mat3d R = m::rotation_zyx(0.1, 0.2, 0.3);
    const m::Vec3d t(1.0, 2.0, 3.0);
    const m::Mat4d T = m::make_transform(R, t);

    CHECK_NEAR(T(3, 3), 1.0, 1e-12);
    CHECK_NEAR(m::transform_translation(T).norm(), t.norm(), 1e-12);

    auto rot = m::transform_rotation(T);
    CHECK(rot.ok());
    CHECK_NEAR((rot.value() - R).norm(), 0.0, 1e-12);

    // 非法矩阵
    m::Mat4d bad = T;
    bad.block<3, 3>(0, 0) *= 2.0;
    CHECK_ERR(m::transform_rotation(bad));

    // 平移部分截断
    m::Mat4d shear = T;
    shear(0, 1) = 0.5;  // 破坏正交性
    CHECK_ERR(m::transform_rotation(shear));
}

MYTEST_CASE(math_solve) {
    m::MatXd A(3, 3);
    A << 4, -2, 1,
        -2, 4, -2,
         1, -2, 4;
    m::VecXd b(3);
    b << 11, -16, 17;

    auto x = m::solve(A, b);
    CHECK_RESULT(x);
    CHECK_NEAR((A * x.value() - b).norm(), 0.0, 1e-9);
    // 校验：4*1 -2*(-2) + 3 = 11 等三式
    CHECK_NEAR(x.value().x(), 1.0, 1e-9);
    CHECK_NEAR(x.value().y(), -2.0, 1e-9);
    CHECK_NEAR(x.value().z(), 3.0, 1e-9);
}

MYTEST_CASE(math_solve_errors) {
    // 非方阵
    m::MatXd rect(2, 3);
    rect.setZero();
    m::VecXd b2(2);
    b2.setZero();
    CHECK_ERR(m::solve(rect, b2));

    // 奇异
    m::MatXd singular(2, 2);
    singular << 1, 2, 2, 4;
    m::VecXd rhs(2);
    rhs << 1, 2;
    CHECK_ERR(m::solve(singular, rhs));

    // 维度不匹配
    m::MatXd I3 = m::MatXd::Identity(3, 3);
    m::VecXd wrong(2);
    wrong.setZero();
    CHECK_ERR(m::solve(I3, wrong));

    // 含 NaN
    m::MatXd nan_mat = m::MatXd::Identity(2, 2);
    nan_mat(0, 0)    = std::nan("");
    m::VecXd b3(2);
    b3.setZero();
    CHECK_ERR(m::solve(nan_mat, b3));
}

MYTEST_CASE(math_least_squares) {
    // y = 2x + 1（无噪声）
    m::MatXd A(4, 2);
    A << 0, 1,
         1, 1,
         2, 1,
         3, 1;
    m::VecXd b(4);
    b << 1, 3, 5, 7;

    auto x = m::least_squares(A, b);
    CHECK_RESULT(x);
    CHECK_NEAR(x.value()[0], 2.0, 1e-9);
    CHECK_NEAR(x.value()[1], 1.0, 1e-9);

    // 行数不足
    m::MatXd tall(2, 3);
    tall.setZero();
    m::VecXd b4(2);
    b4.setZero();
    CHECK_ERR(m::least_squares(tall, b4));
}

MYTEST_CASE(math_condition_number) {
    const double well = m::condition_number(m::MatXd::Identity(3, 3));
    CHECK_NEAR(well, 1.0, 1e-9);

    // 病态矩阵
    m::MatXd hilbert(3, 3);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) hilbert(r, c) = 1.0 / (r + c + 1);
    CHECK(m::condition_number(hilbert) > 100.0);
}

MYTEST_CASE(math_statistics) {
    std::vector<double> v{1, 2, 3, 4, 5};
    CHECK_NEAR(m::mean(v), 3.0, 1e-12);
    CHECK_NEAR(m::median(v), 3.0, 1e-12);

    std::vector<double> even{1, 2, 3, 4};
    CHECK_NEAR(m::median(even), 2.5, 1e-12);

    // median 会排序传入的缓冲区，但便捷重载传的是副本
    std::vector<double> sorted_after{5, 1, 3, 2, 4};
    const double        med = m::median(sorted_after);
    CHECK_NEAR(med, 3.0, 1e-12);
    CHECK_EQ(sorted_after[0], 5.0);  // 原容器未被修改

    std::vector<double> ones{1, 1, 1, 1};
    CHECK_NEAR(m::rms(ones), 1.0, 1e-12);
    CHECK_NEAR(m::standard_deviation(ones), 0.0, 1e-12);

    // 空输入
    std::vector<double> empty;
    CHECK_NEAR(m::mean(empty), 0.0, 1e-12);
    CHECK_NEAR(m::median(empty), 0.0, 1e-12);
}

MYTEST_CASE(math_moving_average) {
    std::vector<double> v(10);
    for (int i = 0; i < 10; ++i) v[i] = i;

    // window=1 原样返回
    auto same = m::moving_average(v, 1);
    CHECK_EQ(same.size(), std::size_t{10});
    for (int i = 0; i < 10; ++i) CHECK_NEAR(same[i], v[i], 1e-12);

    // window=3 在中间区域等于三元均值
    auto smooth = m::moving_average(v, 3);
    CHECK_EQ(smooth.size(), std::size_t{10});
    CHECK_NEAR(smooth[5], 5.0, 1e-12);  // (4+5+6)/3

    // window 大于长度时原样返回
    auto passthrough = m::moving_average(v, 99);
    CHECK_NEAR(passthrough[3], 3.0, 1e-12);

    // 边界不超过最后一个元素
    CHECK(smooth.back() <= 9.0);
}

MYTEST_CASE(math_aligned_vector) {
    m::AlignedVector<m::Vec4d> v;
    for (int i = 0; i < 100; ++i) v.emplace_back(i, i, i, i);
    CHECK_EQ(v.size(), std::size_t{100});
    for (const auto& e : v) {
        CHECK_EQ(e(0), e(3));
    }
}

// -----------------------------------------------------------------------------
//  least_squares / transform / all_finite 的错误路径
// -----------------------------------------------------------------------------
MYTEST_CASE(math_least_squares_rejects_nonfinite_matrix) {
    // 回归：以前把矩阵喂给 Eigen::Ref<VecXd>，Debug 构建会触发 Eigen 断言；
    // 现在必须先走 matrix_all_finite()，返回 InvalidArgument 而不是崩溃/断言。
    m::MatXd A(3, 3);
    A << 1, 2, 3,
         4, 5, 6,
         7, 8, 9;
    m::VecXd b(3);
    b << 1, 2, 3;

    m::MatXd nan_mat = A;
    nan_mat(1, 1)    = std::nan("");
    auto nan_res     = m::least_squares(nan_mat, b);
    CHECK(!nan_res);
    CHECK_EQ(nan_res.status().code(), my::Errc::InvalidArgument);

    m::MatXd inf_mat = A;
    inf_mat(2, 0)    = std::numeric_limits<double>::infinity();
    auto inf_res     = m::least_squares(inf_mat, b);
    CHECK(!inf_res);
    CHECK_EQ(inf_res.status().code(), my::Errc::InvalidArgument);

    // 右端向量含 NaN 同样要拒绝
    m::VecXd bad_b = b;
    bad_b[0]       = std::nan("");
    auto b_res     = m::least_squares(A, bad_b);
    CHECK(!b_res);
    CHECK_EQ(b_res.status().code(), my::Errc::InvalidArgument);
}

MYTEST_CASE(math_least_squares_ok) {
    // y = 2x + 1 的四个无噪声点；列顺序 [1, x] => 解 = [截距, 斜率] = [1, 2]
    m::MatXd A(4, 2);
    A << 1, 0,
         1, 1,
         1, 2,
         1, 3;
    m::VecXd b(4);
    b << 1, 3, 5, 7;

    auto x = m::least_squares(A, b);
    CHECK_RESULT(x);
    CHECK_EQ(x.value().size(), Eigen::Index{2});
    CHECK_NEAR(x.value()[0], 1.0, 1e-12);
    CHECK_NEAR(x.value()[1], 2.0, 1e-12);
    // 精确线性关系下残差应当接近 0
    CHECK_NEAR((A * x.value() - b).norm(), 0.0, 1e-10);
}

MYTEST_CASE(math_transform_rotation_checks) {
    // 单位阵：成功且旋转部分就是单位阵
    const m::Mat4d identity = m::Mat4d::Identity();
    auto identity_rot       = m::transform_rotation(identity);
    CHECK_RESULT(identity_rot);
    CHECK_NEAR((identity_rot.value() - m::Mat3d::Identity()).norm(), 0.0, 1e-12);

    // 非正交（把某个元素改成 2）：InvalidArgument，而不是返回一个坏矩阵
    m::Mat4d scaled = m::Mat4d::Identity();
    scaled(0, 0)    = 2.0;
    auto scaled_rot = m::transform_rotation(scaled);
    CHECK(!scaled_rot);
    CHECK_EQ(scaled_rot.status().code(), my::Errc::InvalidArgument);

    // 平移部分取回
    const m::Mat3d R = m::rotation_zyx(0.1, -0.2, 0.3);
    const m::Vec3d t(1.5, -2.5, 3.5);
    const m::Mat4d T = m::make_transform(R, t);
    const m::Vec3d back = m::transform_translation(T);
    CHECK_NEAR(back.x(), 1.5, 1e-15);
    CHECK_NEAR(back.y(), -2.5, 1e-15);
    CHECK_NEAR(back.z(), 3.5, 1e-15);

    // 旋转部分非单位但合法的矩阵也要放行
    auto legal = m::transform_rotation(T);
    CHECK_RESULT(legal);
    CHECK_NEAR((legal.value() - R).norm(), 0.0, 1e-12);
}

MYTEST_CASE(math_all_finite) {
    m::VecXd v(3);
    v << 1.0, 2.0, 3.0;
    CHECK(m::all_finite(v));

    m::VecXd with_inf = v;
    with_inf[1]       = std::numeric_limits<double>::infinity();
    CHECK(!m::all_finite(with_inf));

    m::VecXd with_nan = v;
    with_nan[2]       = std::nan("");
    CHECK(!m::all_finite(with_nan));

    // 空向量没有非有限值 -> true（数学上是空真）
    const m::VecXd empty;
    CHECK(m::all_finite(empty));
}
