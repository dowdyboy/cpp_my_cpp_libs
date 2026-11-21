#include "my_cpp_libs/cv/image.hpp"

#include "my_cpp_libs/core/path_utf8.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace my::cv {
namespace {

/// 一次预处理的缓冲上限。resize 的目标尺寸、letterbox 画布、f32 张量都受它约束：
/// 这些尺寸来自配置/模型，不能假设调用方给的一定合理。超限返回 OutOfRange，
/// 而不是让 cv::Mat 抛 cv::Exception / bad_alloc 从 Result 边界穿出去。
constexpr std::size_t kMaxBufferBytes = 512u * 1024u * 1024u;

/// 只在**文件名**里找扩展名（目录名带点时不能把目录当作扩展名）。
std::string lower_ext(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    const auto dot   = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

Status check_prepare_size(int width, int height, int channels) {
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) *
                                 static_cast<std::uint64_t>(height);
    const std::uint64_t bytes = pixels * static_cast<std::uint64_t>(channels);
    const std::uint64_t tensor_bytes = bytes * sizeof(float);
    const std::uint64_t cap = static_cast<std::uint64_t>(kMaxBufferBytes);
    if (bytes > cap || tensor_bytes > cap) {
        return Status(Errc::OutOfRange,
                      "预处理缓冲过大: " + std::to_string(width) + "x" + std::to_string(height) +
                          "x" + std::to_string(channels) + "（画布 " + std::to_string(bytes) +
                          " 字节，张量 " + std::to_string(tensor_bytes) + " 字节，上限 " +
                          std::to_string(cap) + "）");
    }
    return Ok();
}

/// 统一成 3 通道，**但不改变颜色顺序**。
///
/// 关键点：4 通道必须用"只丢 alpha"的转换（BGRA2BGR / RGBA2RGB），
/// 而不是统一转成 BGR —— 否则缓冲区顺序被悄悄改掉，下游按
/// norm.src_order 再交换一次，就变成双重交换（R/B 又反回去了）。
/// 顺序的解释权只留给 `image_to_tensor(src_order, dst_order)`。
Result<Image> to_three_channels(const Image& src, my::infer::ColorOrder order) {
    if (src.empty()) {
        return Status(Errc::InvalidArgument, "输入图像为空");
    }
    switch (src.channels()) {
        case 3:
            return src;
        case 4: {
            Image     out;
            const int code = (order == my::infer::ColorOrder::RGB) ? ::cv::COLOR_RGBA2RGB
                                                                   : ::cv::COLOR_BGRA2BGR;
            ::cv::cvtColor(src, out, code);
            return out;
        }
        case 1: {
            Image out;
            // 灰度三通道化后 R=G=B，和顺序无关
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
    if (path.empty()) return Status(Errc::InvalidArgument, "图像路径为空");
    try {
        // 先自己读字节再 imdecode：imread 在 Windows 上用 ANSI 路径打开文件，
        // 中文/日文路径会失败；imdecode 只吃内存，配合 path_utf8 才能跨平台。
        std::ifstream in(my::core::to_path(path), std::ios::binary);
        if (!in) return Status(Errc::IoError, "无法打开图像文件: " + path);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
        if (bytes.empty()) return Status(Errc::ParseError, "图像文件为空: " + path);
        Image img = ::cv::imdecode(bytes, flags);
        if (img.empty()) {
            return Status(Errc::ParseError,
                          "无法解码图像（文件不存在、格式不支持或文件损坏）: " + path);
        }
        return img;
    } catch (const ::cv::Exception& e) {
        return Status(Errc::ParseError, std::string("OpenCV 解码异常: ") + e.what());
    } catch (const std::exception& e) {
        return Status(Errc::RuntimeError, std::string("读取图像失败: ") + e.what());
    }
}

Status save(const std::string& path, const Image& image) {
    if (path.empty()) return Status(Errc::InvalidArgument, "图像路径为空");
    if (image.empty()) return Status(Errc::InvalidArgument, "要保存的图像为空");

    const std::string ext = lower_ext(path);
    if (ext.empty()) {
        return Status(Errc::InvalidArgument, "无法从路径判断图像格式（缺少扩展名）: " + path);
    }

    try {
        // 先编码到内存，再由 std::ofstream(core::to_path) 落盘：imwrite 的路径
        // 在 Windows 上同样按 ANSI 解释，中文/日文路径会失败。
        std::vector<unsigned char> buf;
        if (!::cv::imencode("." + ext, image, buf) || buf.empty()) {
            return Status(Errc::Unsupported, "OpenCV 不支持该扩展名或编码失败: ." + ext);
        }
        std::ofstream out(my::core::to_path(path), std::ios::binary | std::ios::trunc);
        if (!out) return Status(Errc::IoError, "无法写入图像: " + path);
        out.write(reinterpret_cast<const char*>(buf.data()),
                  static_cast<std::streamsize>(buf.size()));
        out.flush();
        if (!out) return Status(Errc::IoError, "写入图像失败: " + path);
        return Ok();
    } catch (const ::cv::Exception& e) {
        return Status(Errc::RuntimeError, std::string("OpenCV 编码异常: ") + e.what());
    } catch (const std::exception& e) {
        return Status(Errc::RuntimeError, std::string("保存图像失败: ") + e.what());
    }
}

Result<PreparedImage> prepare(const Image& src, const PreprocessSpec& spec) {
    if (spec.input_width <= 0 || spec.input_height <= 0) {
        return Status(Errc::InvalidArgument, "目标尺寸非法");
    }
    if (spec.input_channels != 0 && spec.input_channels != 1 && spec.input_channels != 3) {
        return Status(Errc::InvalidArgument, "input_channels 只支持 0/1/3");
    }

    const int planned_channels = (spec.input_channels == 1) ? 1 : 3;
    if (auto st = check_prepare_size(spec.input_width, spec.input_height, planned_channels); !st) {
        return st;
    }

    try {
        auto bgr = to_three_channels(src, spec.norm.src_order);
        if (!bgr) return bgr.status();

        Image working  = bgr.value();
        int   channels = 3;
        if (spec.input_channels == 1) {
            ::cv::cvtColor(bgr.value(), working, ::cv::COLOR_BGR2GRAY);
            channels = 1;
        }

        PreparedImage out;
        const int dst_w = spec.input_width;
        const int dst_h = spec.input_height;

        if (spec.keep_aspect) {
            out.letterbox = my::infer::letterbox_params(working.cols, working.rows, dst_w, dst_h);

            const int new_w =
                std::max(1, static_cast<int>(std::lround(working.cols * out.letterbox.scale)));
            const int new_h =
                std::max(1, static_cast<int>(std::lround(working.rows * out.letterbox.scale)));

            Image resized;
            ::cv::resize(working, resized, ::cv::Size(new_w, new_h), 0.0, 0.0, spec.interpolation);

            // 画布必须新建：::cv::Mat 的 ROI 不保证连续，不能直接喂给 image_to_tensor
            out.image = Image(dst_h, dst_w, channels == 1 ? CV_8UC1 : CV_8UC3, spec.pad_color);
            const int x = std::min(out.letterbox.pad_x, std::max(0, dst_w - new_w));
            const int y = std::min(out.letterbox.pad_y, std::max(0, dst_h - new_h));

            // 与 my::image 保持一致：scale_x/scale_y 用**取整后**的实际比例，
            // 而不是理想比例；这样 unletterbox_box 不会有累积的亚像素偏差。
            out.letterbox.scale_x = static_cast<float>(new_w) / static_cast<float>(working.cols);
            out.letterbox.scale_y = static_cast<float>(new_h) / static_cast<float>(working.rows);
            out.letterbox.scale   = std::min(out.letterbox.scale_x, out.letterbox.scale_y);
            out.letterbox.pad_x   = x;
            out.letterbox.pad_y   = y;

            resized.copyTo(out.image(::cv::Rect(x, y, std::min(new_w, dst_w - x),
                                              std::min(new_h, dst_h - y))));
        } else {
            // 直接拉伸：两个方向的缩放比例不同，必须分别记下来，
            // 否则 unletterbox_boxes() 没法把框还原（见 Letterbox 注释）
            out.letterbox.scale_x = static_cast<float>(dst_w) / static_cast<float>(working.cols);
            out.letterbox.scale_y = static_cast<float>(dst_h) / static_cast<float>(working.rows);
            out.letterbox.scale   = out.letterbox.scale_x;  // stretch 模式下只作参考
            out.letterbox.pad_x   = 0;
            out.letterbox.pad_y   = 0;
            ::cv::resize(working, out.image, ::cv::Size(dst_w, dst_h), 0.0, 0.0, spec.interpolation);
        }

        // 新建的 ::cv::Mat 内存是连续的；ROI 不是，这里必须确认。
        // 不能用 CV_Assert：它内部展开成 cv::error(...)，在 my::cv 命名空间里会被解析错
        if (!out.image.isContinuous()) {
            return Status(Errc::RuntimeError, "内部错误：预处理画布不是连续内存");
        }

        if (auto st = my::infer::image_to_tensor(out.image.data, dst_h, dst_w, channels, spec.norm,
                                                 out.tensor);
            !st) {
            return st;
        }

        out.shape = (spec.norm.layout == my::infer::Layout::NCHW)
                        ? my::infer::Shape{1, channels, dst_h, dst_w}
                        : my::infer::Shape{1, dst_h, dst_w, channels};
        return out;
    } catch (const ::cv::Exception& e) {
        return Status(Errc::RuntimeError, std::string("OpenCV 预处理异常: ") + e.what());
    } catch (const std::exception& e) {
        return Status(Errc::RuntimeError, std::string("预处理失败: ") + e.what());
    }
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
