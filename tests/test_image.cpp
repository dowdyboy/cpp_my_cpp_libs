// =============================================================================
//  my::image 单元测试
//
//  重点覆盖三类事情（都是"错了不报错、只出错误结果"的高发区）：
//    1. 解码/编码：能不能读回来、坏数据给不给错误；
//    2. 几何：letterbox 的 scale/pad 是否精确、逆变换能不能还原原图坐标；
//    3. 颜色与通道：RGB/BGR、4 通道丢 alpha、灰度模型。
// =============================================================================

#include "my_cpp_libs/image/image.hpp"

#include <cstdio>
#include <cstdint>

#include "test_framework.hpp"

MYTEST_SUITE("image");

namespace {

using my::image::Image;

/// 1x1 的红色 PNG（用 Python 的 zlib 现生成，不是 stb 自己写出来的 ——
/// 这样这组用例验证的是"能解码外部产生的字节"，而不是编码解码互相抵消）。
constexpr std::uint8_t kRedPng1x1[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
    0x0C, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
    0x00, 0x03, 0x01, 0x01, 0x00, 0xF7, 0x03, 0x41, 0x43, 0x00, 0x00, 0x00,
    0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
};

Image make_image(int w, int h, int channels) {
    Image img;
    img.width    = w;
    img.height   = h;
    img.channels = channels;
    img.pixels.resize(img.byte_size());
    return img;
}

}  // namespace

// -----------------------------------------------------------------------------
//  解码 / 编码
// -----------------------------------------------------------------------------
MYTEST_CASE(image_decode_external_png) {
    CHECK_OK_VALUE(img, my::image::load_memory(kRedPng1x1, sizeof(kRedPng1x1), 3));

    CHECK_EQ(img.width, 1);
    CHECK_EQ(img.height, 1);
    CHECK_EQ(img.channels, 3);
    CHECK_EQ(img.pixels.size(), 3u);
    CHECK_EQ(img.pixels[0], 255);  // R
    CHECK_EQ(img.pixels[1], 0);    // G
    CHECK_EQ(img.pixels[2], 0);    // B
}

MYTEST_CASE(image_save_load_roundtrip_png) {
    const char* path = "image_roundtrip.png";

    Image src = make_image(3, 2, 3);
    for (std::size_t i = 0; i < src.pixels.size(); ++i) {
        src.pixels[i] = static_cast<std::uint8_t>((i * 37u) & 0xFFu);
    }

    CHECK_OK(my::image::save(path, src));

    CHECK_OK_VALUE(back, my::image::load(path, 3));
    CHECK_EQ(back.width, 3);
    CHECK_EQ(back.height, 2);
    CHECK_EQ(back.channels, 3);
    CHECK_EQ(back.pixels.size(), src.pixels.size());
    for (std::size_t i = 0; i < src.pixels.size(); ++i) {
        CHECK_EQ(back.pixels[i], src.pixels[i]);
    }

    std::remove(path);
}

MYTEST_CASE(image_load_bad_data_reports_parse_error) {
    const std::uint8_t junk[32] = {0x11, 0x22, 0x33};
    auto               r        = my::image::load_memory(junk, sizeof(junk), 3);
    CHECK(!r);
    CHECK_EQ(r.status().code(), my::Errc::ParseError);

    // 空指针/空数据要先被参数校验拦住，不能交给 stb
    CHECK_ERR(my::image::load_memory(nullptr, 0, 3));
}

MYTEST_CASE(image_load_missing_file_reports_io_error) {
    auto r = my::image::load("no_such_dir/no_such_image.png", 3);
    CHECK(!r);
    CHECK_EQ(r.status().code(), my::Errc::IoError);
}

MYTEST_CASE(image_save_unsupported_extension) {
    Image img = make_image(2, 2, 3);
    CHECK_ERR(my::image::save("x.webp", img));
    CHECK_ERR(my::image::save("", img));  // 空路径
}

// -----------------------------------------------------------------------------
//  缩放
// -----------------------------------------------------------------------------
MYTEST_CASE(image_resize_bilinear_2x2_to_4x4) {
    Image src = make_image(2, 2, 1);
    src.pixels = {0, 100, 200, 255};  // (0,0) (1,0) (0,1) (1,1)

    CHECK_OK_VALUE(up, my::image::resize(src, 4, 4));
    CHECK_EQ(up.width, 4);
    CHECK_EQ(up.height, 4);
    CHECK_EQ(up.channels, 1);

    CHECK_EQ(up.pixels[0], 0);                    // 左上角 == 原图左上角
    CHECK_EQ(up.pixels[4 * 3 + 3], 255);          // 右下角 == 原图右下角
    // 权重 (0.1875*100 + 0.1875*200 + 0.0625*255) = 72.1875 -> 72
    CHECK_EQ(up.pixels[4 * 1 + 1], 72);
}

MYTEST_CASE(image_resize_nearest_replicates_labels) {
    Image src = make_image(2, 2, 1);
    src.pixels = {0, 100, 200, 255};

    CHECK_OK_VALUE(up, my::image::resize_nearest(src, 4, 4));

    // 2x2 -> 4x4 的最近邻 = 每个像素铺成 2x2，不允许出现插值出来的新值
    CHECK_EQ(up.pixels[4 * 0 + 0], 0);
    CHECK_EQ(up.pixels[4 * 0 + 1], 0);
    CHECK_EQ(up.pixels[4 * 1 + 0], 0);
    CHECK_EQ(up.pixels[4 * 2 + 2], 255);
    CHECK_EQ(up.pixels[4 * 3 + 3], 255);
    CHECK_EQ(up.pixels[4 * 0 + 3], 100);
    CHECK_EQ(up.pixels[4 * 3 + 0], 200);
}

MYTEST_CASE(image_resize_rejects_bad_size) {
    Image src = make_image(2, 2, 3);
    CHECK_ERR(my::image::resize(src, 0, 4));
    CHECK_ERR(my::image::resize(src, 4, -1));
}

// -----------------------------------------------------------------------------
//  letterbox / 预处理
// -----------------------------------------------------------------------------
MYTEST_CASE(image_prepare_letterbox_geometry) {
    Image src = make_image(100, 50, 3);
    for (auto& p : src.pixels) p = 10;

    my::image::PreprocessSpec spec;
    spec.input_width  = 64;
    spec.input_height = 64;
    spec.keep_aspect  = true;
    spec.pad_value    = 114;
    spec.norm.scale   = 1.0f / 255.0f;

    CHECK_OK_VALUE(prep, my::image::prepare(src, spec));

    // 100x50 等比放进 64x64：缩放 0.64 -> 64x32，上下各补 16
    CHECK_EQ(prep.letterbox.pad_x, 0);
    CHECK_EQ(prep.letterbox.pad_y, 16);
    CHECK_NEAR(prep.letterbox.scale_x, 0.64, 1e-6);
    CHECK_NEAR(prep.letterbox.scale_y, 0.64, 1e-6);

    CHECK_EQ(prep.shape.dims.size(), 4u);
    CHECK_EQ(prep.shape.dims[0], 1);
    CHECK_EQ(prep.shape.dims[1], 3);
    CHECK_EQ(prep.shape.dims[2], 64);
    CHECK_EQ(prep.shape.dims[3], 64);
    CHECK_EQ(prep.tensor.size(), 3u * 64u * 64u);

    // 填充区应该是 pad_value 归一化后的值（114/255），而不是 0
    CHECK_NEAR(prep.tensor[0], 114.0 / 255.0, 1e-5);
    // 图像内容区从 pad_y 开始，值是 10/255
    const std::size_t content = static_cast<std::size_t>(16 * 64 + 0);
    CHECK_NEAR(prep.tensor[content], 10.0 / 255.0, 1e-5);

    // 逆变换必须精确：把整张图对应的框映射回原图，应当正好是 100x50
    float x1 = 0.0f, y1 = 16.0f, x2 = 64.0f, y2 = 48.0f;
    my::infer::unletterbox_box(x1, y1, x2, y2, prep.letterbox);
    CHECK_NEAR(x1, 0.0, 1e-4);
    CHECK_NEAR(y1, 0.0, 1e-4);
    CHECK_NEAR(x2, 100.0, 1e-4);
    CHECK_NEAR(y2, 50.0, 1e-4);
}

MYTEST_CASE(image_prepare_stretch_keeps_two_scales) {
    Image src = make_image(100, 50, 3);

    my::image::PreprocessSpec spec;
    spec.input_width  = 64;
    spec.input_height = 64;
    spec.keep_aspect  = false;

    CHECK_OK_VALUE(prep, my::image::prepare(src, spec));

    CHECK_NEAR(prep.letterbox.scale_x, 0.64, 1e-6);
    CHECK_NEAR(prep.letterbox.scale_y, 1.28, 1e-6);
    CHECK_EQ(prep.letterbox.pad_x, 0);
    CHECK_EQ(prep.letterbox.pad_y, 0);

    // 拉伸模式下两个方向比例不同，逆变换必须分别除
    float x1 = 0.0f, y1 = 0.0f, x2 = 64.0f, y2 = 64.0f;
    my::infer::unletterbox_box(x1, y1, x2, y2, prep.letterbox);
    CHECK_NEAR(x2, 100.0, 1e-4);
    CHECK_NEAR(y2, 50.0, 1e-4);
}

MYTEST_CASE(image_prepare_color_order_rgb_to_bgr) {
    Image src    = make_image(1, 1, 3);
    src.pixels   = {255, 0, 0};  // RGB 的纯红

    my::image::PreprocessSpec spec;
    spec.input_width  = 1;
    spec.input_height = 1;
    spec.keep_aspect  = false;
    spec.norm.src_order = my::infer::ColorOrder::RGB;
    spec.norm.dst_order = my::infer::ColorOrder::BGR;
    spec.norm.scale     = 1.0f / 255.0f;

    CHECK_OK_VALUE(prep, my::image::prepare(src, spec));
    CHECK_EQ(prep.channels, 3);

    // NCHW：第 0 个平面是 B（=0），第 2 个平面是 R（=1）
    CHECK_NEAR(prep.tensor[0], 0.0, 1e-6);
    CHECK_NEAR(prep.tensor[2], 1.0, 1e-6);
}

MYTEST_CASE(image_prepare_gray_model_from_color_input) {
    Image src  = make_image(2, 1, 3);
    // 纯红、纯绿（stb 的灰度系数 77/150/29 >> 8）
    src.pixels = {255, 0, 0, /**/ 0, 255, 0};

    my::image::PreprocessSpec spec;
    spec.input_width  = 2;
    spec.input_height = 1;
    spec.keep_aspect  = false;
    spec.dst_channels = 1;

    CHECK_OK_VALUE(prep, my::image::prepare(src, spec));

    CHECK_EQ(prep.channels, 1);
    CHECK_EQ(prep.shape.dims[1], 1);  // [1,1,H,W]
    CHECK_EQ(prep.tensor.size(), 2u);
    // 灰度用的是整数系数 (77/150/29) >> 8，不是浮点加权，
    // 期望值也按位算，免得"差不多"把系数错误放过去
    const double gray_red   = static_cast<double>((255u * 77u) >> 8) / 255.0;
    const double gray_green = static_cast<double>((255u * 150u) >> 8) / 255.0;
    CHECK_NEAR(prep.tensor[0], gray_red, 1e-6);
    CHECK_NEAR(prep.tensor[1], gray_green, 1e-6);
}

MYTEST_CASE(image_prepare_rgba_drops_alpha) {
    Image rgba    = make_image(1, 1, 4);
    rgba.pixels   = {10, 20, 30, 255};

    my::image::PreprocessSpec spec;
    spec.input_width  = 1;
    spec.input_height = 1;
    spec.keep_aspect  = false;
    spec.norm.scale   = 1.0f;

    CHECK_OK_VALUE(prep, my::image::prepare(rgba, spec));
    CHECK_EQ(prep.channels, 3);
    CHECK_EQ(prep.tensor.size(), 3u);
    CHECK_NEAR(prep.tensor[0], 10.0, 1e-6);
    CHECK_NEAR(prep.tensor[1], 20.0, 1e-6);
    CHECK_NEAR(prep.tensor[2], 30.0, 1e-6);
}

MYTEST_CASE(image_prepare_nhwc_layout) {
    Image src    = make_image(1, 1, 3);
    src.pixels   = {255, 128, 0};

    my::image::PreprocessSpec spec;
    spec.input_width  = 1;
    spec.input_height = 1;
    spec.keep_aspect  = false;
    spec.norm.layout  = my::infer::Layout::NHWC;
    spec.norm.scale   = 1.0f / 255.0f;

    CHECK_OK_VALUE(prep, my::image::prepare(src, spec));
    CHECK_EQ(prep.shape.dims[3], 3);  // [1,H,W,C]
    CHECK_NEAR(prep.tensor[0], 1.0, 1e-6);
    CHECK_NEAR(prep.tensor[2], 0.0, 1e-6);
}

MYTEST_CASE(image_prepare_rejects_bad_arguments) {
    Image empty;
    my::image::PreprocessSpec spec;
    CHECK_ERR(my::image::prepare(empty, spec));

    Image src = make_image(4, 4, 3);
    spec.dst_channels = 2;  // 只支持 0/1/3
    CHECK_ERR(my::image::prepare(src, spec));

    spec.dst_channels = 0;
    spec.input_width  = 0;
    CHECK_ERR(my::image::prepare(src, spec));
}
