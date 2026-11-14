#include "test_framework.hpp"

#include <my_cpp_libs/infer/preprocess.hpp>

#include <cmath>
#include <vector>

using namespace my::infer;

// -----------------------------------------------------------------------------
//  图像 -> 张量
// -----------------------------------------------------------------------------
MYTEST_CASE(preprocess_bgr_to_rgb_nchw) {
    // 2x2 图像，BGR 顺序，像素值故意区分通道
    // 像素 (0,0): B=10 G=20 R=30
    // 像素 (0,1): B=40 G=50 R=60
    // 像素 (1,0): B=70 G=80 R=90
    // 像素 (1,1): B=100 G=110 R=120
    const std::uint8_t src[2 * 2 * 3] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};

    NormalizeSpec spec;
    spec.src_order = ColorOrder::BGR;
    spec.dst_order = ColorOrder::RGB;
    spec.layout    = Layout::NCHW;
    spec.scale     = 1.0f;  // 不缩放，方便手算
    std::vector<float> out;

    CHECK_OK(image_to_tensor(src, 2, 2, 3, spec, out));
    CHECK_EQ(out.size(), std::size_t{12});

    // NCHW: [R 平面 4 个][G 平面 4 个][B 平面 4 个]
    CHECK_NEAR(out[0], 30.0, 1e-6);   // R 第 1 个像素
    CHECK_NEAR(out[1], 60.0, 1e-6);   // R 第 2 个像素
    CHECK_NEAR(out[2], 90.0, 1e-6);   // R 第 3 个
    CHECK_NEAR(out[3], 120.0, 1e-6);  // R 第 4 个

    CHECK_NEAR(out[4], 20.0, 1e-6);   // G 平面
    CHECK_NEAR(out[8], 10.0, 1e-6);   // B 平面
}

MYTEST_CASE(preprocess_nhwc_layout) {
    const std::uint8_t src[1 * 1 * 3] = {1, 2, 3};  // BGR
    NormalizeSpec spec;
    spec.src_order = ColorOrder::BGR;
    spec.dst_order = ColorOrder::RGB;
    spec.layout    = Layout::NHWC;
    spec.scale     = 1.0f;

    std::vector<float> out;
    CHECK_OK(image_to_tensor(src, 1, 1, 3, spec, out));
    CHECK_EQ(out.size(), std::size_t{3});
    CHECK_NEAR(out[0], 3.0, 1e-6);  // R
    CHECK_NEAR(out[1], 2.0, 1e-6);  // G
    CHECK_NEAR(out[2], 1.0, 1e-6);  // B
}

MYTEST_CASE(preprocess_normalization) {
    const std::uint8_t src[1 * 1 * 3] = {255, 128, 0};  // RGB 顺序
    NormalizeSpec spec;
    spec.src_order = ColorOrder::RGB;
    spec.dst_order = ColorOrder::RGB;
    spec.layout    = Layout::NCHW;
    spec.scale     = 1.0f / 255.0f;
    spec.mean      = {0.5f, 0.0f, 0.0f};
    spec.stddev    = {0.5f, 1.0f, 1.0f};

    std::vector<float> out;
    CHECK_OK(image_to_tensor(src, 1, 1, 3, spec, out));
    CHECK_NEAR(out[0], (1.0f - 0.5f) / 0.5f, 1e-6);  // R = (255/255 - 0.5)/0.5 = 1
    CHECK_NEAR(out[1], 128.0f / 255.0f, 1e-6);
    CHECK_NEAR(out[2], 0.0, 1e-6);
}

MYTEST_CASE(preprocess_gray) {
    const std::uint8_t src[2] = {0, 255};
    NormalizeSpec spec;
    spec.scale = 1.0f / 255.0f;
    std::vector<float> out;
    CHECK_OK(image_to_tensor(src, 1, 2, 1, spec, out));
    CHECK_EQ(out.size(), std::size_t{2});
    CHECK_NEAR(out[0], 0.0, 1e-6);
    CHECK_NEAR(out[1], 1.0, 1e-6);
}

MYTEST_CASE(preprocess_errors) {
    std::vector<float> out;
    NormalizeSpec spec;

    CHECK_ERR(image_to_tensor(nullptr, 2, 2, 3, spec, out));
    CHECK_ERR(image_to_tensor(reinterpret_cast<const std::uint8_t*>("x"), 0, 2, 3, spec, out));
    CHECK_ERR(image_to_tensor(reinterpret_cast<const std::uint8_t*>("x"), 1, 1, 2, spec, out));

    spec.stddev = {1.0f, 0.0f, 1.0f};
    const std::uint8_t src[3] = {0, 0, 0};
    CHECK_ERR(image_to_tensor(src, 1, 1, 3, spec, out));
}

MYTEST_CASE(preprocess_hwc_to_chw) {
    const float src[1 * 2 * 3] = {1, 2, 3, 4, 5, 6};  // 2 像素 x 3 通道
    std::vector<float> out;
    CHECK_OK(hwc_to_chw(src, 1, 2, 3, out));
    CHECK_EQ(out.size(), std::size_t{6});
    CHECK_NEAR(out[0], 1.0, 1e-6);
    CHECK_NEAR(out[1], 4.0, 1e-6);
    CHECK_NEAR(out[2], 2.0, 1e-6);
    CHECK_NEAR(out[3], 5.0, 1e-6);
    CHECK_NEAR(out[4], 3.0, 1e-6);
    CHECK_NEAR(out[5], 6.0, 1e-6);
}

// -----------------------------------------------------------------------------
//  letterbox
// -----------------------------------------------------------------------------
MYTEST_CASE(preprocess_letterbox_landscape) {
    // 640x480 -> 640x640：宽度撑满，上下各补 80
    const Letterbox lb = letterbox_params(640, 480, 640, 640);
    CHECK_NEAR(lb.scale, 1.0, 1e-6);
    CHECK_EQ(lb.pad_x, 0);
    CHECK_EQ(lb.pad_y, 80);
}

MYTEST_CASE(preprocess_letterbox_square_to_416) {
    // 1000x500 -> 416x416：scale=0.416，高 208，上下各补 104
    const Letterbox lb = letterbox_params(1000, 500, 416, 416);
    CHECK_NEAR(lb.scale, 0.416, 1e-6);
    CHECK_EQ(lb.pad_x, 0);
    CHECK_EQ(lb.pad_y, 104);
}

MYTEST_CASE(preprocess_letterbox_roundtrip) {
    const Letterbox lb = letterbox_params(1280, 720, 640, 640);
    // 原图中心点 (640,360) 映射到输入图后应当约等于中心
    float x = 640.0f * lb.scale + static_cast<float>(lb.pad_x);
    float y = 360.0f * lb.scale + static_cast<float>(lb.pad_y);
    CHECK_NEAR(x, 320.0, 1.0);
    CHECK_NEAR(y, 320.0, 1.0);

    // 逆变换回到原坐标
    float x1 = 100.0f, y1 = 200.0f, x2 = 300.0f, y2 = 400.0f;
    unletterbox_box(x1, y1, x2, y2, lb);
    CHECK_NEAR(x1, (100.0f - lb.pad_x) / lb.scale, 1e-3);
    CHECK_NEAR(y2, (400.0f - lb.pad_y) / lb.scale, 1e-3);
}

MYTEST_CASE(preprocess_letterbox_invalid) {
    const Letterbox lb = letterbox_params(0, 100, 640, 640);
    CHECK_NEAR(lb.scale, 1.0, 1e-6);  // 原样返回
    CHECK_EQ(lb.pad_x, 0);
}

// -----------------------------------------------------------------------------
//  分类后处理
// -----------------------------------------------------------------------------
MYTEST_CASE(preprocess_softmax) {
    std::vector<float> v{1.0f, 2.0f, 3.0f};
    CHECK_OK(softmax(v.data(), v.size()));
    double sum = 0.0;
    for (float x : v) sum += x;
    CHECK_NEAR(sum, 1.0, 1e-6);
    CHECK(v[2] > v[1] && v[1] > v[0]);

    // 均匀输入 -> 均匀输出
    std::vector<float> u{0.0f, 0.0f, 0.0f, 0.0f};
    CHECK_OK(softmax(u.data(), u.size()));
    for (float x : u) CHECK_NEAR(x, 0.25, 1e-6);

    // 数值稳定：极大值不应溢出
    std::vector<float> big{1000.0f, 1001.0f};
    CHECK_OK(softmax(big.data(), big.size()));
    CHECK(std::isfinite(big[0]) && std::isfinite(big[1]));
    CHECK_NEAR(big[0] + big[1], 1.0, 1e-6);

    CHECK_ERR(softmax(nullptr, 3));
    CHECK_ERR(softmax(big.data(), 0));
}

MYTEST_CASE(preprocess_argmax_topk) {
    const float scores[]{0.1f, 0.9f, 0.3f, 0.7f};
    CHECK_EQ(argmax(scores, 4), std::size_t{1});

    const auto top2 = topk(scores, 4, 2);
    CHECK_EQ(top2.size(), std::size_t{2});
    CHECK_EQ(top2[0].index, std::size_t{1});
    CHECK_EQ(top2[1].index, std::size_t{3});
    CHECK_NEAR(top2[0].score, 0.9, 1e-6);

    // k 大于元素数时截断
    CHECK_EQ(topk(scores, 4, 99).size(), std::size_t{4});
    CHECK_EQ(topk(scores, 4, 0).size(), std::size_t{0});
}

MYTEST_CASE(preprocess_topk_softmax) {
    const float logits[]{2.0f, 1.0f, 0.0f};
    const auto  p = topk_softmax(logits, 3, 2);
    CHECK_EQ(p.size(), std::size_t{2});
    CHECK_EQ(p[0].index, std::size_t{0});
    CHECK(p[0].score > p[1].score);
    CHECK(p[0].score <= 1.0f);
}

// -----------------------------------------------------------------------------
//  检测后处理
// -----------------------------------------------------------------------------
MYTEST_CASE(preprocess_iou) {
    Box a{0, 0, 10, 10, 1.0f, 0};
    Box b{0, 0, 10, 10, 1.0f, 0};
    CHECK_NEAR(iou(a, b), 1.0, 1e-6);

    Box c{20, 20, 30, 30, 1.0f, 0};
    CHECK_NEAR(iou(a, c), 0.0, 1e-6);

    // 部分重叠：交 5x10=50，并 100+100-50=150 -> 1/3
    Box d{5, 0, 15, 10, 1.0f, 0};
    CHECK_NEAR(iou(a, d), 1.0 / 3.0, 1e-6);
}

MYTEST_CASE(preprocess_nms) {
    std::vector<Box> boxes{
        {0, 0, 10, 10, 0.9f, 0},
        {1, 1, 11, 11, 0.8f, 0},   // 与第一个高度重叠 -> 被抑制
        {50, 50, 60, 60, 0.7f, 0}, // 不重叠 -> 保留
        {0, 0, 10, 10, 0.95f, 1},  // 与第一个重叠但类别不同 -> 保留
    };
    const auto keep = nms(boxes, 0.5f);
    CHECK_EQ(keep.size(), std::size_t{3});
    CHECK_NEAR(keep[0].score, 0.95, 1e-6);  // 按分数排序
    CHECK_EQ(keep[0].label, 1);
}

MYTEST_CASE(preprocess_nms_empty) {
    CHECK_EQ(nms({}, 0.5f).size(), std::size_t{0});
    Box single{0, 0, 1, 1, 0.5f, 0};
    CHECK_EQ(nms({single}, 0.5f).size(), std::size_t{1});
}
