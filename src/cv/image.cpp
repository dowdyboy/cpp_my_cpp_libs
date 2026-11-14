#include "my_cpp_libs/cv/image.hpp"

#include <algorithm>
#include <cmath>

namespace my::cv {
namespace {

/// 统一成 3 通道 BGR
Result<Image> to_bgr(const Image& src) {
    if (src.empty()) {
        return Status(Errc::InvalidArgument, "输入图像为空");
    }
    switch (src.channels()) {
        case 3:
            return src;
        case 4: {
            Image out;
            ::cv::cvtColor(src, out, ::cv::COLOR_BGRA2BGR);
            return out;
        }
        case 1: {
            Image out;
            ::cv::cvtColor(src, out, ::cv::COLOR_GRAY2BGR);
            return out;
        }
        default:
            return Status(Errc::Unsupported,
                          "不支持的通道数: " + std::to_string(src.channels()));
    }
}

}  // namespace

Result<Image> load(const std::string& path, int flags) {
    const Image img = ::cv::imread(path, flags);
    if (img.empty()) {
        return Status(Errc::IoError, "无法读取图像（文件不存在或格式不支持）: " + path);
    }
    return img;
}

Status save(const std::string& path, const Image& image) {
    if (image.empty()) {
        return Status(Errc::InvalidArgument, "要保存的图像为空");
    }
    if (!::cv::imwrite(path, image)) {
        return Status(Errc::IoError, "无法写入图像: " + path);
    }
    return Ok();
}

Result<PreparedImage> prepare(const Image& src, const PreprocessSpec& spec) {
    if (spec.input_width <= 0 || spec.input_height <= 0) {
        return Status(Errc::InvalidArgument, "目标尺寸非法");
    }

    auto bgr = to_bgr(src);
    if (!bgr) return bgr.status();

    PreparedImage out;
    const int dst_w = spec.input_width;
    const int dst_h = spec.input_height;

    if (spec.keep_aspect) {
        out.letterbox = my::infer::letterbox_params(bgr.value().cols, bgr.value().rows, dst_w, dst_h);

        const int new_w =
            std::max(1, static_cast<int>(std::lround(bgr.value().cols * out.letterbox.scale)));
        const int new_h =
            std::max(1, static_cast<int>(std::lround(bgr.value().rows * out.letterbox.scale)));

        Image resized;
        ::cv::resize(bgr.value(), resized, ::cv::Size(new_w, new_h), 0.0, 0.0, spec.interpolation);

        // 画布必须新建：::cv::Mat 的 ROI 不保证连续，不能直接喂给 image_to_tensor
        out.image = Image(dst_h, dst_w, CV_8UC3, spec.pad_color);
        const int x = std::min(out.letterbox.pad_x, std::max(0, dst_w - new_w));
        const int y = std::min(out.letterbox.pad_y, std::max(0, dst_h - new_h));
        resized.copyTo(out.image(::cv::Rect(x, y, std::min(new_w, dst_w - x),
                                          std::min(new_h, dst_h - y))));
    } else {
        out.letterbox.scale = std::min(static_cast<float>(dst_w) / bgr.value().cols,
                                       static_cast<float>(dst_h) / bgr.value().rows);
        out.letterbox.pad_x = 0;
        out.letterbox.pad_y = 0;
        ::cv::resize(bgr.value(), out.image, ::cv::Size(dst_w, dst_h), 0.0, 0.0, spec.interpolation);
    }

    // 新建的 ::cv::Mat 内存是连续的；ROI 不是，这里必须确认。
    // 不能用 CV_Assert：它内部展开成 cv::error(...)，在 my::cv 命名空间里会被解析错
    if (!out.image.isContinuous()) {
        return Status(Errc::RuntimeError, "内部错误：预处理画布不是连续内存");
    }

    if (auto st = my::infer::image_to_tensor(out.image.data, dst_h, dst_w, 3, spec.norm,
                                             out.tensor);
        !st) {
        return st;
    }

    out.shape = (spec.norm.layout == my::infer::Layout::NCHW)
                    ? my::infer::Shape{1, 3, dst_h, dst_w}
                    : my::infer::Shape{1, dst_h, dst_w, 3};
    return out;
}

Result<PreparedImage> prepare_file(const std::string& path, const PreprocessSpec& spec) {
    auto img = load(path);
    if (!img) return img.status();
    return prepare(img.value(), spec);
}

void unletterbox_boxes(float* boxes_xyxy, std::size_t count, const my::infer::Letterbox& lb) {
    if (boxes_xyxy == nullptr) return;
    for (std::size_t i = 0; i < count; ++i) {
        float* b = boxes_xyxy + i * 4;
        my::infer::unletterbox_box(b[0], b[1], b[2], b[3], lb);
    }
}

}  // namespace my::cv
