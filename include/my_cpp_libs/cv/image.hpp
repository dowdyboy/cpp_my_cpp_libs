#pragma once
// =============================================================================
//  my::cv —— 图像组件（封装 OpenCV）
//
//  设计取舍：**不重复 OpenCV 已经做好的事**。
//  ::cv::Mat、::cv::resize、::cv::cvtColor 直接用，封装只在"OpenCV 与推理之间"这一层：
//    * 文件 IO 从"返回空 Mat"改成带错误原因的 Result
//    * prepare() 一次完成 resize -> letterbox -> 颜色转换 -> 归一化 -> HWC->CHW
//    * LetterboxInfo 跟着张量一起返回，后处理才能把框还原
//
//  这些都是"错了不报错、只出错误结果"的环节，集中在一处才不会各写各的。
// =============================================================================

#include <cstddef>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "my_cpp_libs/core/status.hpp"
#include "my_cpp_libs/infer/infer.hpp"
#include "my_cpp_libs/infer/preprocess.hpp"

namespace my::cv {

using Image = ::cv::Mat;

// -----------------------------------------------------------------------------
//  文件 IO
// -----------------------------------------------------------------------------
/// 读图失败会给出原因（文件不存在 / 格式不支持），而不是返回一个空 Mat
Result<Image> load(const std::string& path, int flags = ::cv::IMREAD_COLOR);

Status save(const std::string& path, const Image& image);

// -----------------------------------------------------------------------------
//  推理预处理
// -----------------------------------------------------------------------------
struct PreprocessSpec {
    int input_width  = 640;
    int input_height = 640;

    /// true  = 保持宽高比 + 居中填充（letterbox）
    /// false = 直接拉伸到目标尺寸
    bool keep_aspect = true;

    /// 送进模型的通道数：0 = 跟随输入（见下方说明），1 = 灰度模型，3 = 彩色。
    ///
    /// 以前这里写死 3 通道：单通道的工业检测模型在 cv 组件下**根本不可用**。
    /// 现在显式给一个开关；0 表示"输入几通道就几通道"（4 通道的图会丢 alpha 变 3）。
    int input_channels = 0;

    /// 填充颜色（BGR）。训练侧常见的是 114/114/114 或 0
    ::cv::Scalar pad_color{114, 114, 114};

    /// 颜色顺序、归一化、NCHW/NHWC
    ///
    /// 4 通道的 cv::Mat 约定是 BGRA，但相机/解码库也可能给 RGBA。
    /// 这里用 src_order 区分：BGR -> 按 BGRA 处理，RGB -> 按 RGBA 处理。
    my::infer::NormalizeSpec norm;

    int interpolation = ::cv::INTER_LINEAR;
};

struct PreparedImage {
    /// letterbox 之后的图像（调试可视化用）
    Image image;

    /// 几何参数，后处理要用它把框映射回原图
    my::infer::Letterbox letterbox;

    /// 送给模型的连续 float 数据
    std::vector<float> tensor;

    /// 建议的输入形状：[1,C,H,W] 或 [1,H,W,C]
    my::infer::Shape shape;
};

Result<PreparedImage> prepare(const Image& src, const PreprocessSpec& spec);

Result<PreparedImage> prepare_file(const std::string& path, const PreprocessSpec& spec);

/// 把模型坐标系下的框（xyxy 连续排布）映射回原图坐标系
void unletterbox_boxes(float* boxes_xyxy, std::size_t count, const my::infer::Letterbox& lb);

}  // namespace my::cv
