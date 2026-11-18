#include "my_cpp_libs/image/image.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>

#include "stb_image.h"
#include "stb_image_write.h"

namespace my::image {
namespace {

const char* stb_reason() noexcept {
    const char* r = stbi_failure_reason();
    return (r != nullptr && *r != '\0') ? r : "未知原因";
}

std::string lower_ext(const std::string& path) {
    const auto pos = path.find_last_of('.');
    if (pos == std::string::npos) return {};
    std::string ext = path.substr(pos + 1);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

const char* channels_name(int channels) noexcept {
    switch (channels) {
        case 1: return "GRAY";
        case 2: return "GRAY+ALPHA";
        case 3: return "RGB";
        case 4: return "RGBA";
        default: return "?";
    }
}

/// 转 1 通道。系数和 stb 自己的灰度转换一致（77/150/29 >> 8），
/// 这样"解码时直接要 1 通道"和"这里再转"得到同一张图。
Image to_gray(const Image& src, my::infer::ColorOrder order) {
    Image out;
    out.width    = src.width;
    out.height   = src.height;
    out.channels = 1;
    out.pixels.resize(src.pixel_count());

    const int r_index = (order == my::infer::ColorOrder::RGB) ? 0 : 2;
    const int b_index = (order == my::infer::ColorOrder::RGB) ? 2 : 0;
    const auto* src_px = src.pixels.data();
    const std::size_t ch = static_cast<std::size_t>(src.channels);

    for (std::size_t i = 0; i < src.pixel_count(); ++i) {
        const std::uint8_t* p = src_px + i * ch;
        const unsigned y = static_cast<unsigned>(p[r_index]) * 77u +
                           static_cast<unsigned>(p[1]) * 150u +
                           static_cast<unsigned>(p[b_index]) * 29u;
        out.pixels[i] = static_cast<std::uint8_t>(y >> 8);
    }
    return out;
}

/// 转 3 通道（由调用方保证 src 是 1 或 4 通道）。
Image to_three(const Image& src) {
    Image out;
    out.width    = src.width;
    out.height   = src.height;
    out.channels = 3;
    out.pixels.resize(src.pixel_count() * 3);

    const auto* src_px = src.pixels.data();
    if (src.channels == 1) {
        for (std::size_t i = 0; i < src.pixel_count(); ++i) {
            const std::uint8_t v = src_px[i];
            out.pixels[i * 3 + 0] = v;
            out.pixels[i * 3 + 1] = v;
            out.pixels[i * 3 + 2] = v;
        }
    } else {  // 4 通道：丢 alpha，前三个通道原样搬（顺序由 norm.src_order 描述）
        for (std::size_t i = 0; i < src.pixel_count(); ++i) {
            std::memcpy(out.pixels.data() + i * 3, src_px + i * 4, 3);
        }
    }
    return out;
}

Image convert_channels(const Image& src, int dst_channels, my::infer::ColorOrder order) {
    if (src.channels == dst_channels) return src;
    if (dst_channels == 1) return to_gray(src, order);
    if (dst_channels == 3 && (src.channels == 1 || src.channels == 4)) return to_three(src);
    return {};  // 调用方已校验，这里只是兜底
}

/// 双线性 / 最近邻共用的几何准备：源码坐标 -> 目标坐标的映射
struct AxisMap {
    float scale = 1.0f;  // src / dst

    float at(int d) const noexcept { return (static_cast<float>(d) + 0.5f) * scale - 0.5f; }
};

Image resize_impl(const Image& src, int dst_width, int dst_height, bool bilinear) {
    Image out;
    out.width    = dst_width;
    out.height   = dst_height;
    out.channels = src.channels;
    const std::size_t c = static_cast<std::size_t>(src.channels);
    out.pixels.resize(static_cast<std::size_t>(dst_width) * static_cast<std::size_t>(dst_height) * c);

    const AxisMap mx{static_cast<float>(src.width) / static_cast<float>(dst_width)};
    const AxisMap my{static_cast<float>(src.height) / static_cast<float>(dst_height)};
    const float    max_x = static_cast<float>(src.width - 1);
    const float    max_y = static_cast<float>(src.height - 1);
    const auto*    s     = src.pixels.data();

    for (int y = 0; y < dst_height; ++y) {
        const float fy = std::clamp(my.at(y), 0.0f, max_y);
        const int   y0 = static_cast<int>(fy);
        const int   y1 = std::min(y0 + 1, src.height - 1);
        const float wy = fy - static_cast<float>(y0);

        for (int x = 0; x < dst_width; ++x) {
            const float fx = std::clamp(mx.at(x), 0.0f, max_x);
            const int   x0 = static_cast<int>(fx);
            const int   x1 = std::min(x0 + 1, src.width - 1);
            const float wx = fx - static_cast<float>(x0);

            const std::uint8_t* p00 = s + (static_cast<std::size_t>(y0) * src.width + x0) * c;
            const std::uint8_t* p10 = s + (static_cast<std::size_t>(y0) * src.width + x1) * c;
            const std::uint8_t* p01 = s + (static_cast<std::size_t>(y1) * src.width + x0) * c;
            const std::uint8_t* p11 = s + (static_cast<std::size_t>(y1) * src.width + x1) * c;
            std::uint8_t* dst = out.pixels.data() + (static_cast<std::size_t>(y) * dst_width + x) * c;

            if (!bilinear) {
                const std::uint8_t* p = (wy < 0.5f) ? ((wx < 0.5f) ? p00 : p10) : ((wx < 0.5f) ? p01 : p11);
                std::memcpy(dst, p, c);
                continue;
            }

            const float w00 = (1.0f - wx) * (1.0f - wy);
            const float w10 = wx * (1.0f - wy);
            const float w01 = (1.0f - wx) * wy;
            const float w11 = wx * wy;
            for (std::size_t k = 0; k < c; ++k) {
                const float v = w00 * static_cast<float>(p00[k]) + w10 * static_cast<float>(p10[k]) +
                                w01 * static_cast<float>(p01[k]) + w11 * static_cast<float>(p11[k]);
                dst[k] = static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 255.0f)));
            }
        }
    }
    return out;
}

Result<Image> wrap_stb(std::uint8_t* data, int w, int h, int channels_in_file, int requested) {
    if (data == nullptr) {
        return Status(Errc::ParseError, std::string("解码失败: ") + stb_reason());
    }
    Image img;
    img.width    = w;
    img.height   = h;
    img.channels = (requested > 0) ? requested : channels_in_file;
    img.pixels.assign(data, data + img.byte_size());
    stbi_image_free(data);

    if (img.channels != 1 && img.channels != 3 && img.channels != 4) {
        return Status(Errc::Unsupported,
                      "该图是 " + std::to_string(img.channels) +
                          " 通道（灰+alpha），请显式指定 channels=1 或 3");
    }
    return img;
}

}  // namespace

std::string Image::to_string() const {
    return std::string(channels_name(channels)) + " " + std::to_string(width) + "x" +
           std::to_string(height) + " (" + std::to_string(byte_size()) + " bytes)";
}

// -----------------------------------------------------------------------------
//  解码 / 编码
// -----------------------------------------------------------------------------
Result<Image> load(const std::string& path, int channels) {
    if (path.empty()) return Status(Errc::InvalidArgument, "图像路径为空");
    if (channels < 0 || channels > 4) {
        return Status(Errc::InvalidArgument, "channels 只支持 0/1/3/4");
    }

    int w = 0, h = 0, c = 0;
    std::uint8_t* data = stbi_load(path.c_str(), &w, &h, &c, channels);
    if (data == nullptr) {
        return Status(Errc::IoError,
                      "无法读取图像: " + path + "（" + stb_reason() + "）");
    }
    return wrap_stb(data, w, h, c, channels);
}

Result<Image> load_memory(const void* data, std::size_t size, int channels) {
    if (data == nullptr || size == 0) return Status(Errc::InvalidArgument, "图像数据为空");
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return Status(Errc::InvalidArgument, "图像数据过大（stb 的接口是 int 长度）");
    }
    if (channels < 0 || channels > 4) {
        return Status(Errc::InvalidArgument, "channels 只支持 0/1/3/4");
    }

    int w = 0, h = 0, c = 0;
    std::uint8_t* out = stbi_load_from_memory(static_cast<const stbi_uc*>(data),
                                              static_cast<int>(size), &w, &h, &c, channels);
    if (out == nullptr) {
        return Status(Errc::ParseError, std::string("无法解码图像数据（") + stb_reason() + "）");
    }
    return wrap_stb(out, w, h, c, channels);
}

Status save(const std::string& path, const Image& image, int quality) {
    if (path.empty()) return Status(Errc::InvalidArgument, "图像路径为空");
    if (image.empty()) return Status(Errc::InvalidArgument, "要保存的图像为空");
    if (quality < 1 || quality > 100) quality = 90;

    const std::string ext = lower_ext(path);
    const int         stride = image.width * image.channels;
    int               ok     = 0;

    if (ext.empty() || ext == "png") {
        ok = stbi_write_png(path.c_str(), image.width, image.height, image.channels,
                            image.pixels.data(), stride);
    } else if (ext == "jpg" || ext == "jpeg") {
        ok = stbi_write_jpg(path.c_str(), image.width, image.height, image.channels,
                            image.pixels.data(), quality);
    } else if (ext == "bmp") {
        ok = stbi_write_bmp(path.c_str(), image.width, image.height, image.channels,
                            image.pixels.data());
    } else if (ext == "tga") {
        ok = stbi_write_tga(path.c_str(), image.width, image.height, image.channels,
                            image.pixels.data());
    } else {
        return Status(Errc::Unsupported,
                      "不支持的扩展名: ." + ext + "（支持 png/jpg/jpeg/bmp/tga）");
    }

    if (ok == 0) return Status(Errc::IoError, "写入图像失败: " + path);
    return Ok();
}

// -----------------------------------------------------------------------------
//  缩放
// -----------------------------------------------------------------------------
namespace {

/// 输出缓冲上限：resize 会按 dst_w * dst_h * channels 一次性分配。
/// 没有上限时，`resize(src, 1e6, 1e6)` 直接抛 bad_alloc —— 那是从
/// `Result<T>` 的边界穿出去的异常，等于把"错误可返回"的契约废掉了。
constexpr std::size_t kMaxImageBytes = 512u * 1024u * 1024u;  // 512 MiB

Status check_output_size(int width, int height, int channels) {
    if (width <= 0 || height <= 0 || channels <= 0) {
        return Status(Errc::InvalidArgument, "目标尺寸非法");
    }
    const std::uint64_t bytes = static_cast<std::uint64_t>(width) *
                               static_cast<std::uint64_t>(height) *
                               static_cast<std::uint64_t>(channels);
    if (bytes > static_cast<std::uint64_t>(kMaxImageBytes)) {
        return Status(Errc::OutOfRange, "目标图像过大: " + std::to_string(width) + "x" +
                                            std::to_string(height) + "x" +
                                            std::to_string(channels) + " bytes=" +
                                            std::to_string(bytes));
    }
    return Ok();
}

}  // namespace

Result<Image> resize(const Image& src, int dst_width, int dst_height) {
    if (src.empty()) return Status(Errc::InvalidArgument, "输入图像为空");
    if (auto st = check_output_size(dst_width, dst_height, src.channels); !st) return st;
    if (dst_width == src.width && dst_height == src.height) return src;
    return resize_impl(src, dst_width, dst_height, /*bilinear=*/true);
}

Result<Image> resize_nearest(const Image& src, int dst_width, int dst_height) {
    if (src.empty()) return Status(Errc::InvalidArgument, "输入图像为空");
    if (auto st = check_output_size(dst_width, dst_height, src.channels); !st) return st;
    if (dst_width == src.width && dst_height == src.height) return src;
    return resize_impl(src, dst_width, dst_height, /*bilinear=*/false);
}

// -----------------------------------------------------------------------------
//  推理预处理
// -----------------------------------------------------------------------------
Result<PreparedImage> prepare(const Image& src, const PreprocessSpec& spec) {
    if (src.empty()) return Status(Errc::InvalidArgument, "输入图像为空");
    if (spec.input_width <= 0 || spec.input_height <= 0) {
        return Status(Errc::InvalidArgument, "模型输入尺寸非法");
    }
    if (src.channels != 1 && src.channels != 3 && src.channels != 4) {
        return Status(Errc::Unsupported, "只支持 1/3/4 通道的图像");
    }

    int dst_channels = spec.dst_channels;
    if (dst_channels == 0) dst_channels = (src.channels == 4) ? 3 : src.channels;
    if (dst_channels != 1 && dst_channels != 3) {
        return Status(Errc::InvalidArgument, "dst_channels 只支持 0/1/3");
    }

    // 1) 通道转换放在缩放之前：1 通道的图少算 2/3 的插值
    const Image converted = convert_channels(src, dst_channels, spec.norm.src_order);
    if (converted.empty()) {
        return Status(Errc::RuntimeError, "内部错误：通道转换失败");
    }

    // 2) 几何：先算缩放，再按**取整后的实际尺寸**回填 scale_x/scale_y，
    //    这样 unletterbox_box() 不会因为 lround 和理想比例不一致而累积偏差。
    my::infer::Letterbox lb;
    int                  new_w = 0;
    int                  new_h = 0;

    if (spec.keep_aspect) {
        lb    = my::infer::letterbox_params(converted.width, converted.height,
                                            spec.input_width, spec.input_height);
        new_w = std::max(1, static_cast<int>(std::lround(
                                static_cast<float>(converted.width) * lb.scale)));
        new_h = std::max(1, static_cast<int>(std::lround(
                                static_cast<float>(converted.height) * lb.scale)));
        new_w = std::min(new_w, spec.input_width);
        new_h = std::min(new_h, spec.input_height);
        lb.pad_x = (spec.input_width - new_w) / 2;
        lb.pad_y = (spec.input_height - new_h) / 2;
    } else {
        new_w    = spec.input_width;
        new_h    = spec.input_height;
        lb.pad_x = 0;
        lb.pad_y = 0;
    }

    lb.scale_x = static_cast<float>(new_w) / static_cast<float>(converted.width);
    lb.scale_y = static_cast<float>(new_h) / static_cast<float>(converted.height);
    lb.scale   = spec.keep_aspect ? std::min(lb.scale_x, lb.scale_y) : lb.scale_x;

    // 3) 缩放
    auto resized = resize(converted, new_w, new_h);
    if (!resized) return resized.status();

    // 4) 画布 + 居中贴图
    PreparedImage out;
    out.canvas.width    = spec.input_width;
    out.canvas.height   = spec.input_height;
    out.canvas.channels = dst_channels;
    const std::size_t canvas_bytes = static_cast<std::size_t>(spec.input_width) *
                                     static_cast<std::size_t>(spec.input_height) *
                                     static_cast<std::size_t>(dst_channels);
    out.canvas.pixels.assign(canvas_bytes, spec.pad_value);

    // 取整可能让 new_w/new_h 比剩余空间大 1px，夹一下再贴
    const int copy_w = std::min(new_w, spec.input_width - lb.pad_x);
    const int copy_h = std::min(new_h, spec.input_height - lb.pad_y);
    const std::size_t row_bytes = static_cast<std::size_t>(copy_w) * static_cast<std::size_t>(dst_channels);
    for (int y = 0; y < copy_h; ++y) {
        const std::uint8_t* src_row =
            resized.value().pixels.data() + static_cast<std::size_t>(y) * new_w * dst_channels;
        std::uint8_t* dst_row = out.canvas.pixels.data() +
                                (static_cast<std::size_t>(lb.pad_y + y) * spec.input_width + lb.pad_x) *
                                    static_cast<std::size_t>(dst_channels);
        std::memcpy(dst_row, src_row, row_bytes);
    }

    // 5) 归一化 + 布局（颜色顺序在这一步按 spec.norm 处理）
    if (auto st = my::infer::image_to_tensor(out.canvas.pixels.data(), spec.input_height,
                                             spec.input_width, dst_channels, spec.norm, out.tensor);
        !st) {
        return st;
    }

    out.letterbox = lb;
    out.channels  = dst_channels;
    out.shape     = (spec.norm.layout == my::infer::Layout::NCHW)
                        ? my::infer::Shape{1, dst_channels, spec.input_height, spec.input_width}
                        : my::infer::Shape{1, spec.input_height, spec.input_width, dst_channels};
    return out;
}

Result<PreparedImage> prepare_file(const std::string& path, const PreprocessSpec& spec) {
    auto img = load(path, /*channels=*/0);  // 保持原样，由 prepare 决定怎么转
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

}  // namespace my::image
