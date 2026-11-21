#pragma once
// =============================================================================
//  my::image —— 不依赖 OpenCV 的图像组件
//
//  为什么要有它：my::cv 直接绑死 OpenCV，而板端（Zynq armv7）没有 OpenCV，
//  于是"相机 -> 解码 -> 预处理 -> 推理"这条链在板上第一步就断了。
//  这里用 stb_image 补上缺的那一段，只依赖 core + preprocess：
//
//      解码/编码(stb) -> resize/letterbox(本文件) -> 归一化(preprocess.hpp) -> 张量
//
//  与 my::cv 的分工（两者产出的 Letterbox / 张量语义一致，可以互换）：
//    * my::image：任何平台都能编，板端默认用它；
//    * my::cv   ：有 OpenCV 时用，能直接吃 cv::Mat，格式支持更全（HEIF 等）。
//
//  颜色顺序的坑：stb 解码出来是 **RGB**，而 OpenCV 是 BGR。
//  所以 PreprocessSpec::norm 的默认 src_order 在本组件里是 RGB ——
//  照抄 my::cv 的默认值会导致 R/B 静默互换（图看着正常，模型结果全错）。
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "my_cpp_libs/core/status.hpp"
#include "my_cpp_libs/export.hpp"
#include "my_cpp_libs/infer/infer.hpp"
#include "my_cpp_libs/infer/preprocess.hpp"

namespace my::image {

// -----------------------------------------------------------------------------
//  图像容器
// -----------------------------------------------------------------------------
/// HWC、8 bit、逐像素紧密排列（行优先、无 stride）。这是喂给 stb 和预处理
/// 最省事的布局；需要 cv::Mat 时自己包一层（data 可以直接给 cv::Mat 构造）。
struct Image {
    int                       width    = 0;
    int                       height   = 0;
    int                       channels = 0;  ///< 1 / 3 / 4
    std::vector<std::uint8_t> pixels;        ///< size == width * height * channels

    bool        empty() const noexcept { return width <= 0 || height <= 0 || channels <= 0 || pixels.empty(); }
    std::size_t pixel_count() const noexcept {
        return (width > 0 && height > 0) ? static_cast<std::size_t>(width) * static_cast<std::size_t>(height)
                                         : 0u;
    }
    std::size_t byte_size() const noexcept {
        return channels > 0 ? pixel_count() * static_cast<std::size_t>(channels) : 0u;
    }

    /// 形如 "RGB 640x480 (921600 bytes)"
    MYLIB_API std::string to_string() const;
};

/// 解码图像文件。支持 PNG / JPEG / BMP / TGA / GIF（stb 支持的那几种）。
///
/// @param channels 期望通道数：0 = 保持文件原样（仅 1/3/4；灰度+alpha 的 2 通道图
///                 由调用方显式传 1 或 3），1/3/4 = 由 stb 转换。
///                 注意 16 bit 图会被降到 8 bit，本组件不处理高位深。
MYLIB_API Result<Image> load(const std::string& path, int channels = 3);

/// 从内存解码（内嵌资源、无文件系统的板端、网络收到的帧都走这条）。
MYLIB_API Result<Image> load_memory(const void* data, std::size_t size, int channels = 3);

/// 按扩展名编码：.png / .jpg / .jpeg / .bmp / .tga；没有扩展名时按 PNG 写，
/// 其它扩展名返回 Errc::Unsupported（不会静默改格式）。
/// JPEG 会用 quality（1~100），其余格式忽略它。
MYLIB_API Status save(const std::string& path, const Image& image, int quality = 90);

// -----------------------------------------------------------------------------
//  缩放
// -----------------------------------------------------------------------------
/// 双线性缩放（按像素中心对齐，边界夹取）。
///
/// 说明：这是"点采样式"双线性，放大没问题；**大幅下采样（>2x）会有混叠**，
/// 因为没做面积平均。相机链路一般是 640x480 -> 640x640（放大），够用；
/// 如果以后要 4K -> 640，再换 stb_image_resize2 的重采样版本。
MYLIB_API Result<Image> resize(const Image& src, int dst_width, int dst_height);

/// 最近邻缩放（标签图 / 掩码必须用它，双线性会把类别编号插值成不存在的值）。
MYLIB_API Result<Image> resize_nearest(const Image& src, int dst_width, int dst_height);

// -----------------------------------------------------------------------------
//  推理预处理
// -----------------------------------------------------------------------------
struct PreprocessSpec {
    int input_width  = 640;
    int input_height = 640;

    /// true  = 保持宽高比 + 居中填充（letterbox，检测模型必须用它）
    /// false = 直接拉伸到目标尺寸（分类模型常见）
    bool keep_aspect = true;

    /// 填充值（0~255，三通道同值）。训练侧常见 114 或 0，**必须和训练侧一致**。
    std::uint8_t pad_value = 114;

    /// 输出通道数：0 = 跟随输入（4 通道丢 alpha 后变 3，1 通道保持 1）。
    /// 灰度模型（工业检测很常见）用 1。
    int dst_channels = 0;

    /// 颜色顺序 / 归一化 / NCHW-NHWC。
    ///
    /// 默认值和 my::infer::NormalizeSpec 不同：这里把 src_order 设成 **RGB**，
    /// 因为 stb 解码出来就是 RGB。用别的方式拿到缓冲区时记得改成实际顺序。
    infer::NormalizeSpec norm = [] {
        infer::NormalizeSpec s;
        s.src_order = infer::ColorOrder::RGB;
        return s;
    }();
};

struct PreparedImage {
    /// letterbox 之后的图（仅调试/可视化用；约占 input_w*input_h*C 字节，板端别长期持有）
    Image canvas;

    /// 几何参数，后处理要用它把框映射回原图
    infer::Letterbox letterbox;

    /// 送给模型的连续 f32 数据
    std::vector<float> tensor;

    /// 建议的输入形状：[1,C,H,W] 或 [1,H,W,C]
    infer::Shape shape;

    /// 张量通道数（1 或 3），= shape 里对应的那一维
    int channels = 0;
};

/// 解码 -> (通道转换) -> resize -> letterbox 填充 -> 归一化 -> NCHW/NHWC
///
/// 与 cv::prepare 的区别不只是"不依赖 OpenCV"：letterbox 的 scale_x/scale_y
/// 用的是**取整之后**的实际缩放比（而不是理想比例），所以 unletterbox_box()
/// 映射回原图不会累积亚像素偏差。
MYLIB_API Result<PreparedImage> prepare(const Image& src, const PreprocessSpec& spec);

/// 读文件 + prepare。解码失败会带出 stb 给的原因。
MYLIB_API Result<PreparedImage> prepare_file(const std::string& path, const PreprocessSpec& spec);

/// 把模型坐标系下的框（xyxy 连续排布）批量映射回原图坐标系
MYLIB_API void unletterbox_boxes(float* boxes_xyxy, std::size_t count, const infer::Letterbox& lb);

}  // namespace my::image
