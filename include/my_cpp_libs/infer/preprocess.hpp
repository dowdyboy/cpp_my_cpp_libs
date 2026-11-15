#pragma once
// =============================================================================
//  推理预处理 / 后处理（纯数组，不依赖 OpenCV）
//
//  为什么单独拎出来：模型跑通但结果全错，绝大多数时候是这几件事之一
//    * 颜色顺序（BGR vs RGB）
//    * 归一化（/255 还是 (x-mean)/std，mean 是每通道还是标量）
//    * 布局（NCHW vs NHWC）
//    * letterbox 的 padding 算错，导致框位置整体偏移
//  把这些集中在一处实现并测试，比每个工程各写一遍可靠得多。
//
//  接了 OpenCV 之后，这些函数依然是底座（OpenCV 只负责解码和 resize）。
// =============================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "my_cpp_libs/core/status.hpp"
#include "my_cpp_libs/infer/infer.hpp"

namespace my::infer {

enum class ColorOrder { RGB, BGR };
enum class Layout { NCHW, NHWC };

// -----------------------------------------------------------------------------
//  图像 -> 张量
// -----------------------------------------------------------------------------
struct NormalizeSpec {
    ColorOrder src_order = ColorOrder::BGR;  ///< cv::imread 读出来是 BGR
    ColorOrder dst_order = ColorOrder::RGB;  ///< 多数训练脚本用 RGB
    Layout     layout    = Layout::NCHW;     ///< PyTorch 用 NCHW，TF 用 NHWC

    float scale = 1.0f / 255.0f;             ///< 先乘这个
    std::array<float, 3> mean{0.0f, 0.0f, 0.0f};  ///< 再减（按目标通道顺序）
    std::array<float, 3> stddev{1.0f, 1.0f, 1.0f};  ///< 最后除
};

/// HWC 的 u8 图像 -> f32 张量（batch=1）
/// src 长度必须 >= height * width * channels
///
/// channels 支持 1 / 3 / 4：
///   * 1   -> 输出 1 通道（用 mean[0] / stddev[0]）
///   * 3   -> 输出 3 通道
///   * 4   -> **丢弃 alpha**，输出 3 通道（BGRA / RGBA 图片很常见，模型输入一般只要 3 通道）
/// 因此 out.size() 恒为 height * width * min(channels, 3)，不会出现"第 4 个平面全是 0"的半成品。
Status image_to_tensor(const std::uint8_t* src, int height, int width, int channels,
                       const NormalizeSpec& spec, std::vector<float>& out);

/// HWC -> NCHW（f32，原地不可，需要目标缓冲）
Status hwc_to_chw(const float* src, int height, int width, int channels, std::vector<float>& out);

/// f32 图像归一化（不改变布局），用于已经转成 float 的场景。
/// 缓冲按 channels 个通道解释：NCHW 为 [C][pixels]，NHWC 为 [pixels][C]，
/// 因此 count 必须恰好是 pixels * channels。
Status normalize(const float* src, std::size_t pixels, int channels, const NormalizeSpec& spec,
                 std::vector<float>& out);

// -----------------------------------------------------------------------------
//  letterbox
// -----------------------------------------------------------------------------
/// 几何参数：把原图坐标映射到模型输入坐标需要"先缩放、再平移"。
///
/// 两种模式都覆盖：
///   * 等比缩放（letterbox）：scale_x == scale_y == scale，pad_x / pad_y 居中填充；
///   * 直接拉伸（stretch）：scale_x 与 scale_y 通常不同，pad 为 0。
///
/// **不要用 scale 做逆变换**：stretch 模式下它没有意义，一律用 scale_x / scale_y
/// （`unletterbox_box()` 已经这么做）。
struct Letterbox {
    float scale   = 1.0f;  ///< 等比缩放比例；stretch 模式下等于 scale_x（仅供参考）
    float scale_x = 1.0f;  ///< x 方向缩放比例（模型输入宽 / 原图宽）
    float scale_y = 1.0f;  ///< y 方向缩放比例（模型输入高 / 原图高）
    int   pad_x   = 0;     ///< 左侧填充
    int   pad_y   = 0;     ///< 顶部填充
};

/// 计算把 src 等比放进 dst 的缩放与填充（居中）。拉伸模式请自行填 scale_x / scale_y。
Letterbox letterbox_params(int src_width, int src_height, int dst_width, int dst_height) noexcept;

/// 把模型输入坐标系下的框映射回原图坐标系（就地修改，不裁剪到原图边界）
void unletterbox_box(float& x1, float& y1, float& x2, float& y2, const Letterbox& lb) noexcept;

// -----------------------------------------------------------------------------
//  分类后处理
// -----------------------------------------------------------------------------
Status softmax(float* data, std::size_t n);

std::size_t argmax(const float* data, std::size_t n) noexcept;

struct Prediction {
    std::size_t index = 0;
    float       score = 0.0f;
};

/// 取前 k 大（不要求输入已归一化）
std::vector<Prediction> topk(const float* data, std::size_t n, std::size_t k);

/// softmax 后取前 k 大
std::vector<Prediction> topk_softmax(const float* logits, std::size_t n, std::size_t k);

// -----------------------------------------------------------------------------
//  检测后处理
// -----------------------------------------------------------------------------
struct Box {
    float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
    float score = 0.0f;
    int   label = 0;
};

float iou(const Box& a, const Box& b) noexcept;

/// 非极大值抑制。按 score 降序，逐类别做（同 label 才互相抑制）
std::vector<Box> nms(std::vector<Box> boxes, float iou_threshold);

}  // namespace my::infer
