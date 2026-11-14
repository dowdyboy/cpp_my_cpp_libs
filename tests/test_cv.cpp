#include "test_framework.hpp"

#include <my_cpp_libs/cv/image.hpp>
#include <my_cpp_libs/infer/preprocess.hpp>

#include <filesystem>
#include <string>

// 注意：本文件只在 MYLIB_ENABLE_CV=ON 且找到 OpenCV 时参与编译

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

MYTEST_CASE(cv_unletterbox_boxes) {
    const auto lb = my::infer::letterbox_params(160, 120, 32, 32);  // scale 0.2, pad_y 4

    float box[4] = {0.0f, 4.0f, 32.0f, 36.0f};  // 模型坐标系下的整幅图
    my::cv::unletterbox_boxes(box, 1, lb);

    CHECK_NEAR(box[0], 0.0, 1e-3);
    CHECK_NEAR(box[1], 0.0, 1e-3);
    CHECK_NEAR(box[2], 160.0, 1e-3);
    CHECK_NEAR(box[3], 160.0, 1e-3);  // (36-4)/0.2 = 160
}
