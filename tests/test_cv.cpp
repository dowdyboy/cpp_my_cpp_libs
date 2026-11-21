#include "test_framework.hpp"

#include <my_cpp_libs/core/path_utf8.hpp>
#include <my_cpp_libs/cv/image.hpp>
#include <my_cpp_libs/infer/preprocess.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

// 注意：本文件只在 MYLIB_ENABLE_CV=ON 且找到 OpenCV 时参与编译

MYTEST_SUITE("cv");

MYTEST_CASE(cv_save_and_load) {
    std::error_code ec;
    std::filesystem::create_directories("output", ec);

    // 造一张 8x6 的图，左上角一个白点
    my::cv::Image img(6, 8, CV_8UC3, ::cv::Scalar(0, 0, 0));
    img.at<::cv::Vec3b>(0, 0) = ::cv::Vec3b(255, 255, 255);

    CHECK_OK(my::cv::save("output/test_cv.png", img));

    auto loaded = my::cv::load("output/test_cv.png");
    CHECK_RESULT(loaded);
    CHECK_EQ(loaded.value().cols, 8);
    CHECK_EQ(loaded.value().rows, 6);
    CHECK_EQ(loaded.value().channels(), 3);

    // 白点应该还在（PNG 无损）
    const ::cv::Vec3b px = loaded.value().at<::cv::Vec3b>(0, 0);
    CHECK_EQ(static_cast<int>(px[0]), 255);
    CHECK_EQ(static_cast<int>(px[1]), 255);
}

MYTEST_CASE(cv_load_missing) {
    auto r = my::cv::load("output/definitely_not_here.png");
    CHECK(!r.ok());
    CHECK_EQ(r.status().code(), my::Errc::IoError);
}

MYTEST_CASE(cv_prepare_letterbox) {
    // 160x120 -> 32x32：scale=0.2，高 24，上下各补 4
    my::cv::Image img(120, 160, CV_8UC3, ::cv::Scalar(0, 0, 255));  // 纯红

    my::cv::PreprocessSpec spec;
    spec.input_width  = 32;
    spec.input_height = 32;
    spec.keep_aspect  = true;

    auto prepared = my::cv::prepare(img, spec);
    CHECK_RESULT(prepared);

    const auto& p = prepared.value();
    CHECK_NEAR(p.letterbox.scale, 0.2, 1e-6);
    CHECK_EQ(p.letterbox.pad_x, 0);
    CHECK_EQ(p.letterbox.pad_y, 4);

    CHECK_EQ(p.image.cols, 32);
    CHECK_EQ(p.image.rows, 32);

    // NCHW：3 个平面，每平面 32*32
    CHECK_EQ(p.tensor.size(), std::size_t{3 * 32 * 32});
    CHECK_EQ(p.shape.dims.size(), std::size_t{4});
    CHECK_EQ(p.shape.dims[0], std::int64_t{1});
    CHECK_EQ(p.shape.dims[1], std::int64_t{3});
    CHECK_EQ(p.shape.dims[2], std::int64_t{32});

    // 中心像素应该是红的（R=1, G=0, B=0）
    const std::size_t plane = 32 * 32;
    const std::size_t center = 16 * 32 + 16;
    CHECK_NEAR(p.tensor[0 * plane + center], 1.0, 1e-5);  // R
    CHECK_NEAR(p.tensor[1 * plane + center], 0.0, 1e-5);  // G
    CHECK_NEAR(p.tensor[2 * plane + center], 0.0, 1e-5);  // B

    // 顶部 padding 区域应该是填充色 114/255
    CHECK_NEAR(p.tensor[0 * plane + 0], 114.0 / 255.0, 1e-5);
}

MYTEST_CASE(cv_prepare_stretch) {
    my::cv::Image img(120, 160, CV_8UC3, ::cv::Scalar(0, 255, 0));  // 纯绿

    my::cv::PreprocessSpec spec;
    spec.input_width  = 32;
    spec.input_height = 32;
    spec.keep_aspect  = false;

    auto prepared = my::cv::prepare(img, spec);
    CHECK_RESULT(prepared);
    CHECK_EQ(prepared.value().letterbox.pad_x, 0);
    CHECK_EQ(prepared.value().letterbox.pad_y, 0);
    CHECK_EQ(prepared.value().image.cols, 32);

    // 拉伸模式下两个方向的比例必须分别记录：32/160 = 0.2，32/120 ≈ 0.2667
    CHECK_NEAR(prepared.value().letterbox.scale_x, 32.0 / 160.0, 1e-5);
    CHECK_NEAR(prepared.value().letterbox.scale_y, 32.0 / 120.0, 1e-5);

    // 用 prepare 返回的 letterbox 做逆变换，整幅图必须回到 160x120
    float box[4] = {0.0f, 0.0f, 32.0f, 32.0f};
    my::cv::unletterbox_boxes(box, 1, prepared.value().letterbox);
    CHECK_NEAR(box[2], 160.0, 1e-2);
    CHECK_NEAR(box[3], 120.0, 1e-2);
}

MYTEST_CASE(cv_prepare_gray_input) {
    my::cv::Image gray(10, 10, CV_8UC1, ::cv::Scalar(128));

    my::cv::PreprocessSpec spec;
    spec.input_width  = 8;
    spec.input_height = 8;

    auto prepared = my::cv::prepare(gray, spec);
    CHECK_RESULT(prepared);
    CHECK_EQ(prepared.value().image.channels(), 3);  // 转成 3 通道
}

// 灰度模型：以前 prepare 无条件转 3 通道，单通道模型根本喂不进去
MYTEST_CASE(cv_prepare_gray_model) {
    my::cv::Image img(4, 4, CV_8UC3, ::cv::Scalar(10, 20, 30));  // BGR

    my::cv::PreprocessSpec spec;
    spec.input_width  = 4;
    spec.input_height = 4;
    spec.keep_aspect  = false;
    spec.input_channels = 1;  // 模型要 1 通道

    auto prepared = my::cv::prepare(img, spec);
    CHECK_RESULT(prepared);

    CHECK_EQ(prepared.value().image.channels(), 1);
    CHECK_EQ(prepared.value().tensor.size(), std::size_t{16});
    CHECK_EQ(prepared.value().shape.dims[1], std::int64_t{1});
}

// 4 通道缓冲区到底是 BGRA 还是 RGBA：靠 norm.src_order 区分。
// 这条以前是"静默按 BGRA 处理"，RGBA 输入会 R/B 互换且不报错。
MYTEST_CASE(cv_prepare_rgba_order_follows_src_order) {
    my::cv::Image rgba(1, 1, CV_8UC4);
    rgba.at<::cv::Vec4b>(0, 0) = ::cv::Vec4b(10, 20, 30, 255);  // 通道 0/1/2 = 10/20/30

    my::cv::PreprocessSpec spec;
    spec.input_width  = 1;
    spec.input_height = 1;
    spec.keep_aspect  = false;
    spec.norm.scale   = 1.0f / 255.0f;
    spec.norm.dst_order = my::infer::ColorOrder::RGB;

    // 当成 RGBA：R=10 G=20 B=30
    spec.norm.src_order = my::infer::ColorOrder::RGB;
    auto as_rgba = my::cv::prepare(rgba, spec);
    CHECK_RESULT(as_rgba);
    CHECK_NEAR(as_rgba.value().tensor[0], 10.0 / 255.0, 1e-6);  // R
    CHECK_NEAR(as_rgba.value().tensor[2], 30.0 / 255.0, 1e-6);  // B

    // 当成 BGRA：B=10 G=20 R=30 -> 输出 RGB 后 R 应该是 30
    spec.norm.src_order = my::infer::ColorOrder::BGR;
    auto as_bgra = my::cv::prepare(rgba, spec);
    CHECK_RESULT(as_bgra);
    CHECK_NEAR(as_bgra.value().tensor[0], 30.0 / 255.0, 1e-6);  // R
    CHECK_NEAR(as_bgra.value().tensor[2], 10.0 / 255.0, 1e-6);  // B
}

MYTEST_CASE(cv_unletterbox_boxes) {
    const auto lb = my::infer::letterbox_params(160, 120, 32, 32);  // scale 0.2, pad_y 4

    float box[4] = {0.0f, 4.0f, 32.0f, 36.0f};  // 模型坐标系下的整幅图
    my::cv::unletterbox_boxes(box, 1, lb);

    CHECK_NEAR(box[0], 0.0, 1e-3);
    CHECK_NEAR(box[1], 0.0, 1e-3);
    CHECK_NEAR(box[2], 160.0, 1e-3);
    CHECK_NEAR(box[3], 160.0, 1e-3);  // (36-4)/0.2 = 160
}

// 与 my::image 对齐：scale_x/scale_y 用取整后的实际缩放比，而不是理想比例，
// 否则逆变换会有亚像素偏差（头文件承诺两个组件的 Letterbox 语义一致）。
MYTEST_CASE(cv_prepare_letterbox_uses_rounded_scale) {
    my::cv::Image img(77, 101, CV_8UC3, ::cv::Scalar(0, 0, 0));  // 101x77 -> 32x32

    my::cv::PreprocessSpec spec;
    spec.input_width  = 32;
    spec.input_height = 32;
    spec.keep_aspect  = true;

    auto prep = my::cv::prepare(img, spec);
    CHECK_RESULT(prep);

    // new_w=32, new_h=round(77*32/101)=24，实际比例分别为 32/101 与 24/77
    CHECK_NEAR(prep.value().letterbox.scale_x, 32.0 / 101.0, 1e-6);
    CHECK_NEAR(prep.value().letterbox.scale_y, 24.0 / 77.0, 1e-6);
    CHECK_NEAR(prep.value().letterbox.scale,
               std::min(32.0 / 101.0, 24.0 / 77.0), 1e-6);
    CHECK_EQ(prep.value().letterbox.pad_x, 0);
    CHECK_EQ(prep.value().letterbox.pad_y, (32 - 24) / 2);
}

// imread/imwrite 在 Windows 上按 ANSI 打开路径；改为"读字节 + imdecode /
// imencode + 自己落盘"后，中文路径必须可用。
MYTEST_CASE(cv_utf8_path_roundtrip) {
    std::error_code ec;
    const std::string dir = "output/中文图像";
    std::filesystem::create_directories(my::core::to_path(dir), ec);
    CHECK(!ec);

    const std::string path = dir + "/测试.png";
    std::filesystem::remove(my::core::to_path(path), ec);

    my::cv::Image img(6, 8, CV_8UC3, ::cv::Scalar(1, 2, 3));
    CHECK_OK(my::cv::save(path, img));
    CHECK(std::filesystem::exists(my::core::to_path(path)));

    auto loaded = my::cv::load(path);
    CHECK_RESULT(loaded);
    CHECK_EQ(loaded.value().cols, 8);
    CHECK_EQ(loaded.value().rows, 6);
    CHECK_EQ(loaded.value().channels(), 3);

    std::filesystem::remove(my::core::to_path(path), ec);
    std::filesystem::remove(my::core::to_path(dir), ec);
}

// 画布/张量分配必须有上限，不能把 cv::Exception / bad_alloc 抛出 Result。
MYTEST_CASE(cv_prepare_rejects_huge_canvas) {
    my::cv::Image img(1, 1000, CV_8UC3, ::cv::Scalar(0, 0, 0));

    my::cv::PreprocessSpec spec;
    spec.input_width  = 200000;
    spec.input_height = 200000;

    auto r = my::cv::prepare(img, spec);
    CHECK(!r);
    CHECK_EQ(r.status().code(), my::Errc::OutOfRange);
}
