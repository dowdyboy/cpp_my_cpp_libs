#include "my_cpp_libs/math/math.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace my::math {

// -----------------------------------------------------------------------------
//  基础工具
// -----------------------------------------------------------------------------
bool all_finite(const Eigen::Ref<const VecXd>& v) noexcept {
    for (Eigen::Index i = 0; i < v.size(); ++i) {
        if (!std::isfinite(v[i])) return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
//  姿态
// -----------------------------------------------------------------------------
Mat3d rotation_zyx(double roll, double pitch, double yaw) {
    // 直接用闭式公式填矩阵，而不是 AngleAxis 连乘：
    //   1) 省掉多次矩阵乘法
    //   2) R(2,0) 恰好等于 -sin(pitch)，反解时精度更好（万向锁附近尤其明显）
    const double cr = std::cos(roll),  sr = std::sin(roll);
    const double cp = std::cos(pitch), sp = std::sin(pitch);
    const double cy = std::cos(yaw),   sy = std::sin(yaw);

    Mat3d R;
    R << cy * cp, cy * sp * sr - sy * cr, cy * sp * cr + sy * sr,
         sy * cp, sy * sp * sr + cy * cr, sy * sp * cr - cy * sr,
         -sp,     cp * sr,               cp * cr;
    return R;
}

Vec3d euler_zyx(const Mat3d& rotation) noexcept {
    // R = Rz(y)Ry(p)Rx(r)  =>  R(2,0) = -sin(p)
    const double sin_pitch = clamp(-rotation(2, 0), -1.0, 1.0);
    const double pitch     = std::asin(sin_pitch);

    // 万向锁判定：pitch 接近 ±90° 时 roll/yaw 耦合，只剩 (roll-yaw) 可辨识。
    // 阈值取 1e-6（约 6e-5 度）—— 再小的话数值噪声会主导 roll/yaw 的分配。
    if (std::abs(std::cos(pitch)) < 1e-6) {
        const double roll = std::atan2(-rotation(1, 2), rotation(1, 1));
        return Vec3d(roll, pitch, 0.0);
    }

    const double roll = std::atan2(rotation(2, 1), rotation(2, 2));
    const double yaw  = std::atan2(rotation(1, 0), rotation(0, 0));
    return Vec3d(roll, pitch, yaw);
}

Quatd quaternion_zyx(double roll, double pitch, double yaw) noexcept {
    return Quatd(rotation_zyx(roll, pitch, yaw));
}

Vec3d euler_zyx(const Quatd& q) noexcept { return euler_zyx(q.normalized().toRotationMatrix()); }

Mat4d make_transform(const Mat3d& rotation, const Vec3d& translation) {
    Mat4d t = Mat4d::Identity();
    t.block<3, 3>(0, 0) = rotation;
    t.block<3, 1>(0, 3) = translation;
    return t;
}

Result<Mat3d> transform_rotation(const Mat4d& transform) noexcept {
    const Mat3d r = transform.block<3, 3>(0, 0);
    // 合法性校验：正交 + 行列式≈1
    const Mat3d should_be_identity = r.transpose() * r;
    if ((should_be_identity - Mat3d::Identity()).cwiseAbs().maxCoeff() > 1e-6) {
        return Status(Errc::InvalidArgument, "变换矩阵的旋转部分不是正交阵");
    }
    if (std::abs(r.determinant() - 1.0) > 1e-6) {
        return Status(Errc::InvalidArgument, "变换矩阵的旋转部分行列式不为 1");
    }
    return r;
}

Vec3d transform_translation(const Mat4d& transform) noexcept {
    return transform.block<3, 1>(0, 3);
}

namespace {

bool matrix_all_finite(const MatXd& m) noexcept {
    for (Eigen::Index i = 0; i < m.size(); ++i) {
        if (!std::isfinite(m.data()[i])) return false;
    }
    return true;
}

}  // namespace

// -----------------------------------------------------------------------------
//  线性代数
// -----------------------------------------------------------------------------
Result<VecXd> solve(const MatXd& A, const VecXd& b) {
    if (A.rows() != A.cols()) {
        return Status(Errc::InvalidArgument, "solve 要求方阵，实际为 " + std::to_string(A.rows()) +
                                                 "x" + std::to_string(A.cols()));
    }
    if (b.size() != A.rows()) {
        return Status(Errc::InvalidArgument, "右端向量维度与矩阵不匹配");
    }
    if (!matrix_all_finite(A) || !all_finite(b)) {
        return Status(Errc::InvalidArgument, "solve 输入包含非有限值");
    }

    Eigen::FullPivLU<MatXd> lu(A);
    if (!lu.isInvertible()) {
        return Status(Errc::RuntimeError, "矩阵奇异，无法求解");
    }
    VecXd x = lu.solve(b);

    const double residual = (A * x - b).norm();
    const double scale    = std::max(1.0, b.norm());
    if (!std::isfinite(residual) || residual / scale > 1e-6) {
        return Status(Errc::RuntimeError, "求解残差过大: " + std::to_string(residual));
    }
    return x;
}

Result<VecXd> least_squares(const MatXd& A, const VecXd& b) {
    if (A.rows() < A.cols()) {
        return Status(Errc::InvalidArgument, "least_squares 要求行数 >= 列数");
    }
    if (b.size() != A.rows()) {
        return Status(Errc::InvalidArgument, "右端向量维度与矩阵不匹配");
    }
    if (!all_finite(b)) {
        return Status(Errc::InvalidArgument, "least_squares 输入包含非有限值");
    }

    Eigen::ColPivHouseholderQR<MatXd> qr(A);
    if (qr.rank() < A.cols()) {
        return Status(Errc::RuntimeError,
                      "设计矩阵秩亏（rank=" + std::to_string(qr.rank()) + "/" +
                          std::to_string(A.cols()) + "）");
    }
    return VecXd(qr.solve(b));
}

double condition_number(const MatXd& A) {
    if (A.size() == 0) return std::numeric_limits<double>::infinity();
    Eigen::JacobiSVD<MatXd> svd(A);
    const auto& s = svd.singularValues();
    if (s.size() == 0) return std::numeric_limits<double>::infinity();
    const double smax = s(0);
    const double smin = s(s.size() - 1);
    if (!(smin > 0.0) || !std::isfinite(smax)) {
        return std::numeric_limits<double>::infinity();
    }
    return smax / smin;
}

// -----------------------------------------------------------------------------
//  统计
// -----------------------------------------------------------------------------
double mean(const double* data, std::size_t n) noexcept {
    if (data == nullptr || n == 0) return 0.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) sum += data[i];
    return sum / static_cast<double>(n);
}

double median_inplace(std::vector<double>& scratch) noexcept {
    if (scratch.empty()) return 0.0;
    const std::size_t n = scratch.size();
    const std::size_t mid = n / 2;
    std::nth_element(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(mid), scratch.end());
    if (n % 2 == 1) return scratch[mid];
    // 偶数个：取中间两个的平均
    const double upper = scratch[mid];
    const double lower = *std::max_element(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(mid));
    return 0.5 * (lower + upper);
}

double median(std::vector<double> scratch) noexcept { return median_inplace(scratch); }

double rms(const double* data, std::size_t n) noexcept {
    if (data == nullptr || n == 0) return 0.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) sum += data[i] * data[i];
    return std::sqrt(sum / static_cast<double>(n));
}

double standard_deviation(const double* data, std::size_t n) noexcept {
    if (data == nullptr || n < 2) return 0.0;
    const double m = mean(data, n);
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = data[i] - m;
        acc += d * d;
    }
    // 样本标准差（n-1）
    return std::sqrt(acc / static_cast<double>(n - 1));
}

std::vector<double> moving_average(const double* data, std::size_t n, std::size_t window) {
    std::vector<double> out;
    if (data == nullptr || n == 0) return out;
    out.resize(n);

    if (window <= 1 || window > n) {
        std::copy(data, data + n, out.begin());
        return out;
    }

    // 前缀和，O(n)；注意累积误差用 double 足够
    std::vector<double> prefix(n + 1, 0.0);
    for (std::size_t i = 0; i < n; ++i) prefix[i + 1] = prefix[i] + data[i];

    const std::size_t half = window / 2;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t lo = (i > half) ? (i - half) : 0;
        std::size_t       hi = i + (window - half);
        if (hi > n) hi = n;
        const std::size_t count = hi - lo;
        out[i] = (prefix[hi] - prefix[lo]) / static_cast<double>(count);
    }
    return out;
}

}  // namespace my::math
