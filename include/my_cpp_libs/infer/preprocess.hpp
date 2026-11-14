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
Status image_to_tensor(const std::uint8_t* src, int height, int width, int channels,
                       const NormalizeSpec& spec, std::vector<float>& out);

/// HWC -> NCHW（f32，原地不可，需要目标缓冲）
Status hwc_to_chw(const float* src, int height, int width, int channels, std::vector<float>& out);

/// f32 图像归一化（不改变布局），用于已经转成 float 的场景
Status normalize(const float* src, std::size_t count, const NormalizeSpec& spec,
                 std::vector<float>& out);

// -----------------------------------------------------------------------------
//  letterbox
// -----------------------------------------------------------------------------
struct Letterbox {
    float scale = 1.0f;  ///< 缩放比例（等比）
    int   pad_x = 0;     ///< 左侧填充
    int   pad_y = 0;     ///< 顶部填充
};

/// 计算把 src 等比放进 dst 的缩放与填充（居中）
Letterbox letterbox_params(int src_width, int src_height, int dst_width, int dst_height) noexcept;

/// 把模型输入坐标系下的框映射回原图坐标系（就地修改）
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
