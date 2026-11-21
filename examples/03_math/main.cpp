// =============================================================================
//  示例 03：数学
//  演示 my::math：姿态转换、线性方程组、最小二乘、统计与滑窗滤波。
// =============================================================================

#include <my_cpp_libs/log/format.hpp>
#include <my_cpp_libs/log/log.hpp>
#include <my_cpp_libs/math/math.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

void format_vec(const char* tag, const my::math::Vec3d& v) {
    my::log::info("{}: [{:.4f}, {:.4f}, {:.4f}] 度", tag, my::math::rad2deg(v.x()),
                  my::math::rad2deg(v.y()), my::math::rad2deg(v.z()));
}

}  // namespace

int main() {
    my::log::Config lc;
    lc.level = my::log::Level::Info;
    lc.use_color = true;
    (void)my::log::init(lc);

    // =========================================================================
    //  1) 姿态：欧拉角 <-> 旋转矩阵 <-> 四元数
    // =========================================================================
    const double roll  = my::math::deg2rad(10.0);
    const double pitch = my::math::deg2rad(-20.0);
    const double yaw   = my::math::deg2rad(30.0);

    my::math::Mat3d R = my::math::rotation_zyx(roll, pitch, yaw);
    my::math::Vec3d euler_back = my::math::euler_zyx(R);

    format_vec("原始欧拉角", my::math::Vec3d(roll, pitch, yaw));
    format_vec("矩阵反解回", euler_back);
    my::log::info("往返误差 = {:.3e}",
                  (euler_back - my::math::Vec3d(roll, pitch, yaw)).norm());

    my::math::Quatd q = my::math::quaternion_zyx(roll, pitch, yaw);
    my::log::info("四元数 = [{:.4f}, {:.4f}, {:.4f}, {:.4f}] (w,x,y,z)", q.w(), q.x(), q.y(), q.z());
    my::log::info("由四元数取回 yaw = {:.2f} 度",
                  my::math::rad2deg(my::math::euler_zyx(q).z()));

    // =========================================================================
    //  2) 齐次变换矩阵
    // =========================================================================
    my::math::Mat4d T = my::math::make_transform(R, my::math::Vec3d(1.0, 2.0, 3.0));
    my::log::info("变换矩阵:\n{}", [&] {
        std::string s;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) s += my::log::format("{:8.4f} ", T(r, c));
            s += "\n";
        }
        return s;
    }());

    auto rot_back = my::math::transform_rotation(T);
    if (rot_back) {
        my::log::info("取回旋转部分误差 = {:.3e}", (rot_back.value() - R).norm());
    }
    // 非法变换矩阵会被拒绝
    my::math::Mat4d bad = T;
    bad.block<3, 3>(0, 0) *= 2.0;
    if (auto st = my::math::transform_rotation(bad); !st) {
        my::log::warn("预期失败: {}", st.status().to_string());
    }

    // =========================================================================
    //  3) 线性方程组 A x = b
    // =========================================================================
    my::math::MatXd A(3, 3);
    A << 4, -2, 1,
        -2, 4, -2,
         1, -2, 4;
    my::math::VecXd b(3);
    b << 11, -16, 17;

    auto x = my::math::solve(A, b);
    if (x) {
        my::log::info("solve 结果 = [{:.4f}, {:.4f}, {:.4f}]", x.value()[0], x.value()[1],
                      x.value()[2]);
        my::log::info("残差 = {:.3e}", (A * x.value() - b).norm());
    }

    // 奇异矩阵 -> 明确的失败
    my::math::MatXd singular(2, 2);
    singular << 1, 2, 2, 4;
    my::math::VecXd rhs(2);
    rhs << 1.0, 2.0;
    if (auto st = my::math::solve(singular, rhs); !st) {
        my::log::warn("预期失败: {}", st.status().to_string());
    }

    // =========================================================================
    //  4) 最小二乘拟合 y = a x + b
    // =========================================================================
    std::mt19937                           rng(42);
    std::normal_distribution<double>       noise(0.0, 0.5);
    std::vector<double>                    xs, ys;
    const double                           true_a = 2.5, true_b = -1.0;
    for (int i = 0; i < 50; ++i) {
        const double xv = i * 0.2;
        xs.push_back(xv);
        ys.push_back(true_a * xv + true_b + noise(rng));
    }

    my::math::MatXd design(xs.size(), 2);
    my::math::VecXd obs(static_cast<Eigen::Index>(ys.size()));
    for (std::size_t i = 0; i < xs.size(); ++i) {
        design(static_cast<Eigen::Index>(i), 0) = xs[i];
        design(static_cast<Eigen::Index>(i), 1) = 1.0;
        obs(static_cast<Eigen::Index>(i))       = ys[i];
    }

    auto fit = my::math::least_squares(design, obs);
    if (fit) {
        my::log::info("拟合 y = {:.4f} x + {:.4f}  (真值 {:.1f} x + {:.1f})", fit.value()[0],
                      fit.value()[1], true_a, true_b);
    }

    // =========================================================================
    //  5) 统计与滑窗滤波（模拟传感器数据）
    // =========================================================================
    std::normal_distribution<double> sensor(100.0, 2.0);
    std::vector<double>              raw;
    raw.reserve(200);
    for (int i = 0; i < 200; ++i) raw.push_back(sensor(rng));
    raw[50] = 300.0;  // 一个明显的野值

    my::log::info("原始: mean={:.3f} median={:.3f} std={:.3f} rms={:.3f}",
                  my::math::mean(raw), my::math::median(raw),
                  my::math::standard_deviation(raw), my::math::rms(raw));

    const auto smooth = my::math::moving_average(raw, 15);
    my::log::info("15 点滑动平均：野值 300 被拉回 {:.1f}", smooth[50]);

    // 去掉野值所在区间后，平滑对正常点的影响应该很小
    double max_dev = 0.0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (i >= 42 && i <= 58) continue;  // 跳过被野值污染的窗口
        max_dev = std::max(max_dev, std::abs(smooth[i] - raw[i]));
    }
    my::log::info("正常点上的最大改变量 = {:.2f}（来自原始噪声本身，信号没被抹平）", max_dev);

    my::log::shutdown();
    return 0;
}
