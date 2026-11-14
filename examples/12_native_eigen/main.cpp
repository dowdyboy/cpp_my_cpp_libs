// =============================================================================
//  示例 12：Eigen 原生 API
//
//  覆盖日常最常用的部分：构造、基本运算、分块、分解、最小二乘、几何变换，
//  以及 ARM/Zynq 上用 Eigen 需要注意的对齐问题。
// =============================================================================

#include <Eigen/Core>
#include <Eigen/Dense>
#include <Eigen/Geometry>

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string to_string(const Eigen::MatrixXd& m) {
    std::ostringstream os;
    os << m;
    return os.str();
}

}  // namespace

int main() {
    // =========================================================================
    //  1) 构造与初始化
    // =========================================================================
    const Eigen::Matrix3d I = Eigen::Matrix3d::Identity();
    const Eigen::Matrix3d Z = Eigen::Matrix3d::Zero();
    const Eigen::Matrix3d C = Eigen::Matrix3d::Constant(2.0);
    (void)I;
    (void)Z;
    (void)C;

    Eigen::MatrixXd m(2, 3);
    m << 1, 2, 3,
         4, 5, 6;  // 逗号初始化按行书写

    const Eigen::Vector3d v(1.0, 2.0, 3.0);
    const Eigen::VectorXd lin = Eigen::VectorXd::LinSpaced(5, 0.0, 1.0);  // 0,0.25,0.5,0.75,1

    std::printf("1) m (2x3) =\n%s\n", to_string(m).c_str());
    std::printf("   LinSpaced(5,0,1) = ");
    for (Eigen::Index i = 0; i < lin.size(); ++i) std::printf("%.2f ", lin(i));
    std::printf("\n   v.size=%ld v.norm=%.4f\n", static_cast<long>(v.size()), v.norm());

    // =========================================================================
    //  2) 基本运算
    // =========================================================================
    const Eigen::Vector3d w(3.0, 2.0, 1.0);
    const Eigen::Vector3d n = v.normalized();
    std::printf("2) dot=%.3f  cross=(%.1f,%.1f,%.1f)  normalized=(%.3f,%.3f,%.3f)\n", v.dot(w),
                v.cross(w).x(), v.cross(w).y(), v.cross(w).z(), n.x(), n.y(), n.z());

    Eigen::Matrix3d B;
    B << 4, -2, 1,
        -2, 4, -2,
         1, -2, 4;
    std::printf("   det=%.3f trace=%.1f transpose(0,1)=%.1f\n", B.determinant(), B.trace(),
                B.transpose()(0, 1));

    // 按元素运算：需要 .array()
    const Eigen::ArrayXXd arr = B.array();
    std::printf("3) 逐元素平方之和=%.2f  cwiseAbs 最大值=%.2f\n", (arr * arr).sum(),
                B.cwiseAbs().maxCoeff());

    // =========================================================================
    //  3) 分块与切片（图像/矩阵分块最常用）
    // =========================================================================
    Eigen::Matrix4d T = Eigen::Matrix4d::Identity();
    T.block<3, 3>(0, 0) = Eigen::AngleAxisd(0.3, Eigen::Vector3d::UnitZ()).toRotationMatrix();
    T.block<3, 1>(0, 3) = Eigen::Vector3d(1, 2, 3);

    const Eigen::Matrix3d R = T.block<3, 3>(0, 0);
    const Eigen::Vector3d t = T.block<3, 1>(0, 3);
    const Eigen::Vector3d p = R * Eigen::Vector3d(0, 0, 0) + t;
    std::printf("4) 变换 (0,0,0) -> (%.2f, %.2f, %.2f)\n", p.x(), p.y(), p.z());

    // =========================================================================
    //  4) 分解：LU / QR / SVD / LLT
    // =========================================================================
    Eigen::VectorXd rhs(3);
    rhs << 11, -16, 17;

    const Eigen::VectorXd x_lu  = B.partialPivLu().solve(rhs);
    const Eigen::VectorXd x_qr  = B.colPivHouseholderQr().solve(rhs);
    const Eigen::VectorXd x_svd = B.jacobiSvd<Eigen::ComputeThinU | Eigen::ComputeThinV>().solve(rhs);
    std::printf("5) LU  = (%.4f, %.4f, %.4f)\n", x_lu.x(), x_lu.y(), x_lu.z());
    std::printf("   QR  = (%.4f, %.4f, %.4f)\n", x_qr.x(), x_qr.y(), x_qr.z());
    std::printf("   SVD = (%.4f, %.4f, %.4f)\n", x_svd.x(), x_svd.y(), x_svd.z());

    // 对称正定矩阵用 LLT 最快
    const Eigen::Matrix3d S = B * B.transpose() + Eigen::Matrix3d::Identity();
    const Eigen::Vector3d x_llt = S.llt().solve(rhs);
    std::printf("   LLT = (%.4f, %.4f, %.4f)\n", x_llt.x(), x_llt.y(), x_llt.z());

    // =========================================================================
    //  5) 最小二乘（超定方程）与伪逆
    // =========================================================================
    Eigen::MatrixXd design(5, 2);
    design << 0, 1,
              1, 1,
              2, 1,
              3, 1,
              4, 1;
    Eigen::VectorXd obs(5);
    obs << 1.0, 3.5, 6.1, 8.4, 10.9;

    const Eigen::Vector2d coef = design.colPivHouseholderQr().solve(obs);  // y = a x + b
    std::printf("6) 最小二乘 y = %.4f x + %.4f\n", coef(0), coef(1));

    // =========================================================================
    //  6) 几何：四元数 / 欧拉角 / 角轴
    // =========================================================================
    const Eigen::Quaterniond q =
        Eigen::AngleAxisd(0.5, Eigen::Vector3d::UnitZ()) * Eigen::AngleAxisd(0.2, Eigen::Vector3d::UnitY());
    // Eigen 5 起 eulerAngles() 已弃用，改用 canonicalEulerAngles()
    const Eigen::Vector3d euler = q.toRotationMatrix().canonicalEulerAngles(2, 1, 0);  // yaw,pitch,roll
    std::printf("7) quat=(%.4f,%.4f,%.4f,%.4f) euler(zyx)=(%.3f,%.3f,%.3f) rad\n", q.w(), q.x(),
                q.y(), q.z(), euler(0), euler(1), euler(2));

    const Eigen::Matrix4d Tinv = T.inverse();
    std::printf("   4x4 逆: T * T^-1 的 (0,3) = %.3e\n", (T * Tinv)(0, 3));

    // =========================================================================
    //  7) ARM/Zynq 使用注意
    // =========================================================================
    //  · 定长可向量化类型（Vector4d / Matrix4d / Quaterniond ...）放进 std::vector
    //    必须用对齐分配器：
    std::vector<Eigen::Vector4d, Eigen::aligned_allocator<Eigen::Vector4d>> aligned_vec;
    aligned_vec.emplace_back(1, 2, 3, 4);
    std::printf("8) 对齐分配器示例 size=%zu\n", aligned_vec.size());
    //  · 开 NEON：交叉编译加 -mfpu=neon -mfloat-abi=hard（见 cmake/toolchains/）
    //  · 若出现对齐相关的崩溃，用 -DEIGEN_MAX_ALIGN_BYTES=16（或 0）兜底定位

    return 0;
}
