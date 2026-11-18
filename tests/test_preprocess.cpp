#include "test_framework.hpp"

#include <my_cpp_libs/infer/preprocess.hpp>

#include <cmath>
#include <limits>
#include <vector>

using namespace my::infer;

MYTEST_SUITE("preprocess");

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

// 4 通道输入必须丢弃 alpha 并**输出 3 通道**：
// 以前的实现按 4 通道分配缓冲，NCHW 下第 4 个平面恒为 0，
// NHWC 下步长与 4 不一致 -> 数值整体错位，且不会报任何错。
MYTEST_CASE(preprocess_bgra_drops_alpha) {
    // 2 个像素，BGRA
    const std::uint8_t src[2 * 4] = {10, 20, 30, 255, 40, 50, 60, 128};

    NormalizeSpec spec;
    spec.src_order = ColorOrder::BGR;
    spec.dst_order = ColorOrder::RGB;
    spec.layout    = Layout::NCHW;
    spec.scale     = 1.0f;

    std::vector<float> out;
    CHECK_OK(image_to_tensor(src, 1, 2, 4, spec, out));
    CHECK_EQ(out.size(), std::size_t{6});  // 2 像素 × 3 通道

    // [R 平面][G 平面][B 平面]
    CHECK_NEAR(out[0], 30.0, 1e-6);
    CHECK_NEAR(out[1], 60.0, 1e-6);
    CHECK_NEAR(out[2], 20.0, 1e-6);
    CHECK_NEAR(out[3], 50.0, 1e-6);
    CHECK_NEAR(out[4], 10.0, 1e-6);
    CHECK_NEAR(out[5], 40.0, 1e-6);
}

MYTEST_CASE(preprocess_rgba_nhwc_drops_alpha) {
    // 2 个像素，RGBA，NHWC 排布
    const std::uint8_t src[2 * 4] = {1, 2, 3, 4, 5, 6, 7, 8};

    NormalizeSpec spec;
    spec.src_order = ColorOrder::RGB;
    spec.dst_order = ColorOrder::RGB;
    spec.layout    = Layout::NHWC;
    spec.scale     = 1.0f;

    std::vector<float> out;
    CHECK_OK(image_to_tensor(src, 1, 2, 4, spec, out));
    CHECK_EQ(out.size(), std::size_t{6});
    // 像素 0 的 RGB 要紧挨着，不能按 4 的步长错位
    CHECK_NEAR(out[0], 1.0, 1e-6);
    CHECK_NEAR(out[1], 2.0, 1e-6);
    CHECK_NEAR(out[2], 3.0, 1e-6);
    CHECK_NEAR(out[3], 5.0, 1e-6);
    CHECK_NEAR(out[4], 6.0, 1e-6);
    CHECK_NEAR(out[5], 7.0, 1e-6);
}

// 归一化必须按 layout 找通道：
// 老实现按 i % 3 取 mean/stddev，对 NCHW 数据等于把均值按像素轮转施加。
MYTEST_CASE(preprocess_normalize_nchw) {
    // 2 像素 3 通道，NCHW：[R0 R1][G0 G1][B0 B1]
    const float src[6] = {10, 20, 30, 40, 50, 60};

    NormalizeSpec spec;
    spec.layout = Layout::NCHW;
    spec.scale  = 1.0f;
    spec.mean   = {1.0f, 2.0f, 3.0f};
    spec.stddev = {2.0f, 4.0f, 5.0f};

    std::vector<float> out;
    CHECK_OK(normalize(src, 2, 3, spec, out));
    CHECK_EQ(out.size(), std::size_t{6});

    CHECK_NEAR(out[0], (10.0f - 1.0f) / 2.0f, 1e-6);
    CHECK_NEAR(out[1], (20.0f - 1.0f) / 2.0f, 1e-6);
    CHECK_NEAR(out[2], (30.0f - 2.0f) / 4.0f, 1e-6);
    CHECK_NEAR(out[3], (40.0f - 2.0f) / 4.0f, 1e-6);
    CHECK_NEAR(out[4], (50.0f - 3.0f) / 5.0f, 1e-6);
    CHECK_NEAR(out[5], (60.0f - 3.0f) / 5.0f, 1e-6);
}

MYTEST_CASE(preprocess_normalize_nhwc) {
    // 2 像素 3 通道，NHWC：[RGB0][RGB1]
    const float src[6] = {10, 30, 50, 20, 40, 60};

    NormalizeSpec spec;
    spec.layout = Layout::NHWC;
    spec.scale  = 1.0f;
    spec.mean   = {1.0f, 2.0f, 3.0f};
    spec.stddev = {2.0f, 4.0f, 5.0f};

    std::vector<float> out;
    CHECK_OK(normalize(src, 2, 3, spec, out));
    CHECK_EQ(out.size(), std::size_t{6});

    CHECK_NEAR(out[0], (10.0f - 1.0f) / 2.0f, 1e-6);
    CHECK_NEAR(out[1], (30.0f - 2.0f) / 4.0f, 1e-6);
    CHECK_NEAR(out[2], (50.0f - 3.0f) / 5.0f, 1e-6);
    CHECK_NEAR(out[3], (20.0f - 1.0f) / 2.0f, 1e-6);
    CHECK_NEAR(out[4], (40.0f - 2.0f) / 4.0f, 1e-6);
    CHECK_NEAR(out[5], (60.0f - 3.0f) / 5.0f, 1e-6);
}

MYTEST_CASE(preprocess_normalize_errors) {
    const float src[3] = {1, 2, 3};
    NormalizeSpec spec;
    spec.scale = 1.0f;
    std::vector<float> out;

    CHECK_ERR(normalize(nullptr, 3, 1, spec, out));
    CHECK_ERR(normalize(src, 0, 1, spec, out));
    CHECK_ERR(normalize(src, 3, 0, spec, out));
    CHECK_ERR(normalize(src, 3, 4, spec, out));

    // 只有被实际用到的通道才校验 stddev
    spec.stddev = {1.0f, 0.0f, 1.0f};
    CHECK_ERR(normalize(src, 3, 2, spec, out));
    CHECK_OK(normalize(src, 3, 1, spec, out));
    CHECK_EQ(out.size(), std::size_t{3});
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

// 拉伸（keep_aspect=false）模式下 x/y 比例不同，只有一个 scale 是还原不出来的。
MYTEST_CASE(preprocess_letterbox_stretch_roundtrip) {
    Letterbox lb;
    lb.scale_x = 32.0f / 160.0f;  // 0.2
    lb.scale_y = 32.0f / 120.0f;  // 0.2666...
    lb.scale   = lb.scale_x;
    lb.pad_x   = 0;
    lb.pad_y   = 0;

    // 模型坐标系下的整幅图应当还原成 160x120
    float x1 = 0.0f, y1 = 0.0f, x2 = 32.0f, y2 = 32.0f;
    unletterbox_box(x1, y1, x2, y2, lb);
    CHECK_NEAR(x1, 0.0, 1e-3);
    CHECK_NEAR(y1, 0.0, 1e-3);
    CHECK_NEAR(x2, 160.0, 1e-3);
    CHECK_NEAR(y2, 120.0, 1e-3);  // 老实现会给出 32/0.2 = 160，y 方向整体错 1.33 倍

    // 等比模式下三个 scale 字段必须自洽
    const Letterbox eq = letterbox_params(1280, 720, 640, 640);
    CHECK_NEAR(eq.scale_x, eq.scale, 1e-6);
    CHECK_NEAR(eq.scale_y, eq.scale, 1e-6);

    // 比例非法时不能做除法（保持原值，不产生 inf/nan）
    Letterbox bad;
    bad.scale_x = 0.0f;
    bad.scale_y = 0.0f;
    float a = 1.0f, b = 2.0f, c = 3.0f, d = 4.0f;
    unletterbox_box(a, b, c, d, bad);
    CHECK_NEAR(a, 1.0, 1e-9);
    CHECK_NEAR(d, 4.0, 1e-9);
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

// -----------------------------------------------------------------------------
//  数值边界 / 重复框
// -----------------------------------------------------------------------------
// 极端量级：先减最大值再 exp，1e30 级输入不能上溢出成 inf/inf = NaN。
MYTEST_CASE(preprocess_softmax_extreme_values) {
    std::vector<float> v{1e30f, -1e30f, 0.0f};
    CHECK_OK(softmax(v.data(), v.size()));
    for (float x : v) CHECK(std::isfinite(x));
    CHECK_NEAR(v[0], 1.0, 1e-6);  // exp(1e30-1e30) = 1
    CHECK_NEAR(v[1], 0.0, 1e-6);  // exp(-2e30) 下溢成 0
    CHECK_NEAR(v[2], 0.0, 1e-6);  // exp(-1e30) 下溢成 0
    double sum = 0.0;
    for (float x : v) sum += x;
    CHECK_NEAR(sum, 1.0, 1e-6);

    // 全相等且巨大：平移后全 0 -> 均匀分布（直接 exp(1e30) 会变成 inf/inf = NaN）
    std::vector<float> u{1e30f, 1e30f, 1e30f, 1e30f};
    CHECK_OK(softmax(u.data(), u.size()));
    for (float x : u) {
        CHECK(std::isfinite(x));
        CHECK_NEAR(x, 0.25, 1e-6);
    }

    // NaN 输入：归一化因子非有限 -> RuntimeError（而不是静默写回 NaN 并报成功）
    std::vector<float> nan_v{1.0f, std::numeric_limits<float>::quiet_NaN()};
    const auto         st = softmax(nan_v.data(), nan_v.size());
    CHECK(!st.ok());
    CHECK_EQ(st.code(), my::Errc::RuntimeError);
}

// 重复框：IoU=1，第二个必须被抑制；同一位置不同类别则不互相抑制。
// 注意 nms 的第二个参数是 **IoU 门限**，API 没有 max_output 参数：
// 传 0 表示"只要重叠（IoU>0）就抑制"，不是"最多输出 0 个"。
MYTEST_CASE(preprocess_nms_duplicate_boxes) {
    std::vector<Box> dup{
        {0, 0, 10, 10, 0.5f, 0},
        {0, 0, 10, 10, 0.9f, 0},
    };
    const auto keep = nms(dup, 0.5f);
    CHECK_EQ(keep.size(), std::size_t{1});
    CHECK_NEAR(keep[0].score, 0.9, 1e-6);  // 排序后保留分数高的那个
    CHECK_EQ(keep[0].label, 0);

    // iou_threshold = 0：部分重叠的被抑制，不重叠的两个都留下
    std::vector<Box> mixed{
        {0, 0, 10, 10, 0.9f, 0},
        {5, 5, 15, 15, 0.8f, 0},    // IoU = 25/175 > 0 -> 抑制
        {50, 50, 60, 60, 0.7f, 0},  // 不重叠 -> 保留
    };
    const auto keep0 = nms(mixed, 0.0f);
    CHECK_EQ(keep0.size(), std::size_t{2});
    CHECK_NEAR(keep0[0].score, 0.9, 1e-6);
    CHECK_NEAR(keep0[1].score, 0.7, 1e-6);

    // 完全相同的框但类别不同：逐类别 NMS，两个都保留
    std::vector<Box> cross{
        {0, 0, 10, 10, 0.9f, 0},
        {0, 0, 10, 10, 0.8f, 1},
    };
    CHECK_EQ(nms(cross, 0.5f).size(), std::size_t{2});

    // 空输入配 0 门限也不能出问题
    CHECK_EQ(nms({}, 0.0f).size(), std::size_t{0});
}

// 归一化参数校验的实际边界：
//   * NormalizeSpec.mean/stddev 是 std::array<float,3>，长度在编译期固定，
//     所以不存在"mean 长度不匹配"这种运行期错误；真正的长度约束是 channels<=3。
//   * scale == 0 **不被拒绝**（实现只校验 stddev 非 0），结果是 (0-mean)/stddev 的常数图。
MYTEST_CASE(preprocess_normalize_zero_scale) {
    const float src[3] = {10.0f, 20.0f, 30.0f};

    NormalizeSpec spec;
    spec.layout = Layout::NCHW;
    spec.scale  = 0.0f;
    spec.mean   = {1.0f, 2.0f, 3.0f};
    spec.stddev = {2.0f, 4.0f, 5.0f};

    std::vector<float> out;
    CHECK_OK(normalize(src, /*pixels=*/1, /*channels=*/3, spec, out));  // scale=0 合法
    CHECK_EQ(out.size(), std::size_t{3});
    CHECK_NEAR(out[0], -0.5, 1e-6);  // (10*0 - 1) / 2
    CHECK_NEAR(out[1], -0.5, 1e-6);  // (20*0 - 2) / 4
    CHECK_NEAR(out[2], -0.6, 1e-6);  // (30*0 - 3) / 5

    // 实际的"长度/不良参数"校验：channels > 3 越界
    CHECK_EQ(normalize(src, 3, 4, spec, out).code(), my::Errc::InvalidArgument);
    CHECK_EQ(normalize(src, 3, 0, spec, out).code(), my::Errc::InvalidArgument);

    // stddev 为 0 必须拒绝（只校验实际用到的通道）
    NormalizeSpec zero_std = spec;
    zero_std.stddev        = {0.0f, 1.0f, 1.0f};
    CHECK_EQ(normalize(src, 3, 1, zero_std, out).code(), my::Errc::InvalidArgument);
    // 注意：normalize 按 pixels*channels 读 src，channels=2 时需要 6 个 float。
    // 这里必须给足长度的缓冲区，否则就是测试自己制造栈越界（ASan 抓到过）。
    const float src6[6] = {10.0f, 20.0f, 30.0f, 40.0f, 50.0f, 60.0f};
    zero_std.stddev     = {1.0f, 1.0f, 0.0f};
    CHECK_OK(normalize(src6, 3, 2, zero_std, out));  // 第 3 个通道没用到 -> 不校验
    CHECK_EQ(out.size(), std::size_t{6});
}

