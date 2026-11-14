#include "my_cpp_libs/infer/preprocess.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace my::infer {
namespace {

/// 目标通道 i 应该从源图的哪个通道取
/// dst_order 为 RGB 时，i=0 要 R；src_order 为 BGR 时 R 在索引 2
int source_channel_for(int dst_index, ColorOrder src, ColorOrder dst) noexcept {
    if (src == dst) return dst_index;
    // 只有两种顺序，互为反转
    return 2 - dst_index;
}

}  // namespace

// -----------------------------------------------------------------------------
//  图像 -> 张量
// -----------------------------------------------------------------------------
Status image_to_tensor(const std::uint8_t* src, int height, int width, int channels,
                       const NormalizeSpec& spec, std::vector<float>& out) {
    if (src == nullptr) return Status(Errc::InvalidArgument, "图像数据为空");
    if (height <= 0 || width <= 0) return Status(Errc::InvalidArgument, "图像尺寸非法");
    if (channels != 1 && channels != 3 && channels != 4) {
        return Status(Errc::InvalidArgument, "只支持 1/3/4 通道的图像");
    }
    for (float s : spec.stddev) {
        if (s == 0.0f) return Status(Errc::InvalidArgument, "归一化 stddev 不能为 0");
    }

    const std::size_t pixel_count = static_cast<std::size_t>(height) * static_cast<std::size_t>(width);
    const std::size_t plane       = pixel_count;

    out.assign(pixel_count * static_cast<std::size_t>(channels), 0.0f);

    const bool nchw = (spec.layout == Layout::NCHW);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t p = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                  static_cast<std::size_t>(x);
            const std::uint8_t* px = src + p * static_cast<std::size_t>(channels);

            if (channels == 1) {
                const float v =
                    (static_cast<float>(px[0]) * spec.scale - spec.mean[0]) / spec.stddev[0];
                out[p] = v;
                continue;
            }

            for (int c = 0; c < 3; ++c) {
                const int sc = source_channel_for(c, spec.src_order, spec.dst_order);
                const float v =
                    (static_cast<float>(px[sc]) * spec.scale - spec.mean[c]) / spec.stddev[c];
                const std::size_t dst = nchw ? (static_cast<std::size_t>(c) * plane + p)
                                             : (p * 3 + static_cast<std::size_t>(c));
                out[dst] = v;
            }
        }
    }
    return Ok();
}

Status hwc_to_chw(const float* src, int height, int width, int channels, std::vector<float>& out) {
    if (src == nullptr) return Status(Errc::InvalidArgument, "输入为空");
    if (height <= 0 || width <= 0 || channels <= 0) {
        return Status(Errc::InvalidArgument, "尺寸参数非法");
    }
    const std::size_t plane = static_cast<std::size_t>(height) * static_cast<std::size_t>(width);
    out.assign(plane * static_cast<std::size_t>(channels), 0.0f);

    for (std::size_t p = 0; p < plane; ++p) {
        for (int c = 0; c < channels; ++c) {
            out[static_cast<std::size_t>(c) * plane + p] =
                src[p * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)];
        }
    }
    return Ok();
}

Status normalize(const float* src, std::size_t count, const NormalizeSpec& spec,
                 std::vector<float>& out) {
    if (src == nullptr || count == 0) return Status(Errc::InvalidArgument, "输入为空");
    for (float s : spec.stddev) {
        if (s == 0.0f) return Status(Errc::InvalidArgument, "归一化 stddev 不能为 0");
    }
    out.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t c = i % 3;
        out[i] = (src[i] * spec.scale - spec.mean[c]) / spec.stddev[c];
    }
    return Ok();
}

// -----------------------------------------------------------------------------
//  letterbox
// -----------------------------------------------------------------------------
Letterbox letterbox_params(int src_width, int src_height, int dst_width, int dst_height) noexcept {
    Letterbox lb;
    if (src_width <= 0 || src_height <= 0 || dst_width <= 0 || dst_height <= 0) return lb;

    const float sx = static_cast<float>(dst_width) / static_cast<float>(src_width);
    const float sy = static_cast<float>(dst_height) / static_cast<float>(src_height);
    lb.scale = std::min(sx, sy);

    const int new_w = static_cast<int>(std::lround(static_cast<float>(src_width) * lb.scale));
    const int new_h = static_cast<int>(std::lround(static_cast<float>(src_height) * lb.scale));
    lb.pad_x = (dst_width - new_w) / 2;
    lb.pad_y = (dst_height - new_h) / 2;
    return lb;
}

void unletterbox_box(float& x1, float& y1, float& x2, float& y2, const Letterbox& lb) noexcept {
    if (lb.scale <= 0.0f) return;
    x1 = (x1 - static_cast<float>(lb.pad_x)) / lb.scale;
    y1 = (y1 - static_cast<float>(lb.pad_y)) / lb.scale;
    x2 = (x2 - static_cast<float>(lb.pad_x)) / lb.scale;
    y2 = (y2 - static_cast<float>(lb.pad_y)) / lb.scale;
}

// -----------------------------------------------------------------------------
//  分类
// -----------------------------------------------------------------------------
Status softmax(float* data, std::size_t n) {
    if (data == nullptr || n == 0) return Status(Errc::InvalidArgument, "输入为空");

    float mx = data[0];
    for (std::size_t i = 1; i < n; ++i) mx = std::max(mx, data[i]);

    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        data[i] = std::exp(data[i] - mx);
        sum += data[i];
    }
    if (sum <= 0.0 || !std::isfinite(sum)) {
        return Status(Errc::RuntimeError, "softmax 归一化因子非法");
    }
    for (std::size_t i = 0; i < n; ++i) data[i] = static_cast<float>(data[i] / sum);
    return Ok();
}

std::size_t argmax(const float* data, std::size_t n) noexcept {
    if (data == nullptr || n == 0) return 0;
    return static_cast<std::size_t>(std::max_element(data, data + n) - data);
}

std::vector<Prediction> topk(const float* data, std::size_t n, std::size_t k) {
    std::vector<Prediction> out;
    if (data == nullptr || n == 0 || k == 0) return out;
    k = std::min(k, n);

    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) out.push_back(Prediction{i, data[i]});

    std::partial_sort(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(k), out.end(),
                      [](const Prediction& a, const Prediction& b) { return a.score > b.score; });
    out.resize(k);
    return out;
}

std::vector<Prediction> topk_softmax(const float* logits, std::size_t n, std::size_t k) {
    std::vector<Prediction> out;
    if (logits == nullptr || n == 0 || k == 0) return out;

    std::vector<float> probs(logits, logits + n);
    if (!softmax(probs.data(), probs.size())) return out;
    return topk(probs.data(), probs.size(), k);
}

// -----------------------------------------------------------------------------
//  检测
// -----------------------------------------------------------------------------
float iou(const Box& a, const Box& b) noexcept {
    const float ix1 = std::max(a.x1, b.x1);
    const float iy1 = std::max(a.y1, b.y1);
    const float ix2 = std::min(a.x2, b.x2);
    const float iy2 = std::min(a.y2, b.y2);

    const float iw = ix2 - ix1;
    const float ih = iy2 - iy1;
    if (iw <= 0.0f || ih <= 0.0f) return 0.0f;

    const float inter = iw * ih;
    const float area_a = std::max(0.0f, a.x2 - a.x1) * std::max(0.0f, a.y2 - a.y1);
    const float area_b = std::max(0.0f, b.x2 - b.x1) * std::max(0.0f, b.y2 - b.y1);
    const float uni = area_a + area_b - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

std::vector<Box> nms(std::vector<Box> boxes, float iou_threshold) {
    std::vector<Box> keep;
    if (boxes.empty()) return keep;

    std::sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) { return a.score > b.score; });

    std::vector<char> suppressed(boxes.size(), 0);
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        if (suppressed[i]) continue;
        keep.push_back(boxes[i]);
        for (std::size_t j = i + 1; j < boxes.size(); ++j) {
            if (suppressed[j]) continue;
            if (boxes[j].label != boxes[i].label) continue;  // 不同类别不互相抑制
            if (iou(boxes[i], boxes[j]) > iou_threshold) suppressed[j] = 1;
        }
    }
    return keep;
}

}  // namespace my::infer
