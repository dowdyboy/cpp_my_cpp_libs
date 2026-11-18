#pragma once
// =============================================================================
//  my::math —— 统一数学接口（底层 Eigen 5）
//
//  设计目标
//    * 统一常用类型别名，避免每个项目重新 typedef
//    * 姿态表示统一：欧拉角一律 弧度 + ZYX 内旋（roll/pitch/yaw）
//    * 线性代数/统计的失败路径走 Result，不吞错误
//  注意：Eigen 是 header-only，本头会引入较多模板，属已知成本。
// =============================================================================

#include <cstddef>
#include <vector>

#include <Eigen/Core>
#include <Eigen/Dense>
#include <Eigen/Geometry>

#include "my_cpp_libs/core/status.hpp"
#include "my_cpp_libs/export.hpp"

namespace my::math {

// =============================================================================
//  类型别名
// =============================================================================
using Vec2d = Eigen::Vector2d;
using Vec3d = Eigen::Vector3d;
using Vec4d = Eigen::Vector4d;
using VecXd = Eigen::VectorXd;

using Vec2f = Eigen::Vector2f;
using Vec3f = Eigen::Vector3f;

using Mat2d = Eigen::Matrix2d;
using Mat3d = Eigen::Matrix3d;
using Mat4d = Eigen::Matrix4d;
using MatXd = Eigen::MatrixXd;

using Quatd = Eigen::Quaterniond;
using AngleAxisd = Eigen::AngleAxisd;

/// 需要放进 std::vector 的定长 Eigen 类型必须用对齐分配器
template <class T>
using AlignedVector = std::vector<T, Eigen::aligned_allocator<T>>;

/// 常用的 4x4 变换矩阵容器
using TransformList = AlignedVector<Mat4d>;

constexpr double kPi = 3.14159265358979323846;

// =============================================================================
//  标量工具
// =============================================================================
inline double deg2rad(double deg) noexcept { return deg * (kPi / 180.0); }
inline double rad2deg(double rad) noexcept { return rad * (180.0 / kPi); }

template <class T>
constexpr T clamp(T v, T lo, T hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

template <class T>
constexpr T lerp(T a, T b, double t) noexcept {
    return static_cast<T>(a + (b - a) * t);
}

MYLIB_API bool all_finite(const Eigen::Ref<const VecXd>& v) noexcept;

// =============================================================================
//  姿态：欧拉角 <-> 旋转矩阵 <-> 四元数
//
//  约定：R = Rz(yaw) * Ry(pitch) * Rx(roll)，等价于"内旋 ZYX / 外旋 XYZ"。
//        范围 roll∈[-π,π], pitch∈[-π/2,π/2], yaw∈[-π,π]。
// =============================================================================
MYLIB_API Mat3d rotation_zyx(double roll, double pitch, double yaw);

/// 从旋转矩阵反解欧拉角（自动处理万向锁）
MYLIB_API Vec3d euler_zyx(const Mat3d& rotation) noexcept;

/// 同一个旋转的四元数表示
MYLIB_API Quatd quaternion_zyx(double roll, double pitch, double yaw) noexcept;

MYLIB_API Vec3d euler_zyx(const Quatd& q) noexcept;

/// 由旋转 + 平移构造齐次变换矩阵
MYLIB_API Mat4d make_transform(const Mat3d& rotation, const Vec3d& translation);

/// 由齐次变换矩阵取旋转/平移
/// 注意：不标 noexcept —— 失败路径要构造带消息的 Status（会分配），
/// 标了 noexcept 就等于"内存不足时直接 terminate"，把可返回的错误变成崩溃。
MYLIB_API Result<Mat3d> transform_rotation(const Mat4d& transform);
MYLIB_API Vec3d     transform_translation(const Mat4d& transform) noexcept;

// =============================================================================
//  线性代数
// =============================================================================
/// 方阵线性方程组 A x = b
MYLIB_API Result<VecXd> solve(const MatXd& A, const VecXd& b);

/// 最小二乘 A x ≈ b（要求行数 >= 列数）
MYLIB_API Result<VecXd> least_squares(const MatXd& A, const VecXd& b);

/// 矩阵条件数（用于判断病态程度；奇异返回 +inf）
MYLIB_API double condition_number(const MatXd& A);

// =============================================================================
//  统计（面向裸缓冲区，便于直接喂传感器数据）
// =============================================================================
MYLIB_API double mean(const double* data, std::size_t n) noexcept;

/// 会原地排序传入的缓冲区；不想要副作用请用下面的 median()
MYLIB_API double median_inplace(std::vector<double>& scratch) noexcept;

/// 传值（内部拷贝），不修改调用方数据
MYLIB_API double median(std::vector<double> scratch) noexcept;

MYLIB_API double rms(const double* data, std::size_t n) noexcept;
MYLIB_API double standard_deviation(const double* data, std::size_t n) noexcept;

/// 一维滑动平均；window<=1 时原样返回
MYLIB_API std::vector<double> moving_average(const double* data, std::size_t n, std::size_t window);

// ---- 便捷重载：std::vector<double> ----
inline double mean(const std::vector<double>& v) noexcept { return mean(v.data(), v.size()); }
inline double rms(const std::vector<double>& v) noexcept { return rms(v.data(), v.size()); }
inline double standard_deviation(const std::vector<double>& v) noexcept {
    return standard_deviation(v.data(), v.size());
}
inline std::vector<double> moving_average(const std::vector<double>& v, std::size_t window) {
    return moving_average(v.data(), v.size(), window);
}

// ---- 便捷重载：Eigen 向量 ----
inline double mean(const VecXd& v) noexcept { return mean(v.data(), static_cast<std::size_t>(v.size())); }
inline double rms(const VecXd& v) noexcept { return rms(v.data(), static_cast<std::size_t>(v.size())); }
inline double standard_deviation(const VecXd& v) noexcept {
    return standard_deviation(v.data(), static_cast<std::size_t>(v.size()));
}

}  // namespace my::math
