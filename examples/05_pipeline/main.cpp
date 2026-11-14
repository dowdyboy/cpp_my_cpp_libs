// =============================================================================
//  示例 05：完整推理流水线
//
//  合成一张 32x32 的图 -> 预处理（BGR→RGB、归一化、HWC→NCHW）
//  -> 模型推理 -> 分类后处理（softmax + top-k）
//
//  这是把 my::infer 的各个零件串起来的最小完整例子。
//  接上 OpenCV 之后，只有"合成图像"这一步会换成 cv::imread。
//
//  用法：
//      example_pipeline [model.onnx]
//      不带参数时用 stub 后端（回显），用来验证链路本身通不通。
// =============================================================================

#include <my_cpp_libs/core/build_info.hpp>
#include <my_cpp_libs/infer/infer.hpp>
#include <my_cpp_libs/infer/preprocess.hpp>
#include <my_cpp_libs/log/format.hpp>
#include <my_cpp_libs/log/log.hpp>

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

namespace {

constexpr int kWidth  = 32;
constexpr int kHeight = 32;
constexpr int kChannels = 3;

/// 合成一张"左红右蓝、上下渐变"的图（BGR 顺序，模拟 cv::imread 的输出）
std::vector<std::uint8_t> make_synthetic_image() {
    std::vector<std::uint8_t> img(static_cast<std::size_t>(kWidth) * kHeight * kChannels, 0);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * kWidth + x) * kChannels;
            const auto        u = [](double v) { return static_cast<std::uint8_t>(v * 255.0 + 0.5); };
            img[i + 0] = u(1.0 - static_cast<double>(x) / kWidth);      // B
            img[i + 1] = u(static_cast<double>(y) / kHeight);           // G
            img[i + 2] = u(static_cast<double>(x) / kWidth);            // R
        }
    }
    return img;
}

}  // namespace

int main(int argc, char** argv) {
    my::log::Config lc;
    lc.level = my::log::Level::Info;
    my::log::init(lc);

    my::log::info("{}", my::BuildInfo::summary());

    // -------------------------------------------------------------------------
    //  1) 准备输入：一张图
    // -------------------------------------------------------------------------
    const auto image = make_synthetic_image();
    my::log::info("合成图像: {}x{}x{} = {} 字节", kWidth, kHeight, kChannels, image.size());

    // -------------------------------------------------------------------------
    //  2) 预处理：颜色顺序 + 归一化 + 布局
    //     训练时用的什么参数，这里就必须是什么参数
    // -------------------------------------------------------------------------
    my::infer::NormalizeSpec spec;
    spec.src_order = my::infer::ColorOrder::BGR;   // cv::imread 是 BGR
    spec.dst_order = my::infer::ColorOrder::RGB;   // 训练脚本多半是 RGB
    spec.layout    = my::infer::Layout::NCHW;      // PyTorch 约定
    spec.scale     = 1.0f / 255.0f;
    spec.mean      = {0.0f, 0.0f, 0.0f};
    spec.stddev    = {1.0f, 1.0f, 1.0f};

    std::vector<float> tensor;
    if (auto st = my::infer::image_to_tensor(image.data(), kHeight, kWidth, kChannels, spec, tensor);
        !st) {
        my::log::error("预处理失败: {}", st.to_string());
        return 1;
    }
    my::log::info("预处理完成: {} 个 float（{} 字节）", tensor.size(),
                  tensor.size() * sizeof(float));

    // 抽查几个值，确认颜色通道没搞反
    const std::size_t plane = static_cast<std::size_t>(kWidth) * kHeight;
    my::log::info("左上角 RGB = ({:.3f}, {:.3f}, {:.3f})",
                  tensor[0 * plane + 0], tensor[1 * plane + 0], tensor[2 * plane + 0]);

    // -------------------------------------------------------------------------
    //  3) 推理
    // -------------------------------------------------------------------------
    my::infer::Options opt;
    if (argc > 1) opt.model_path = argv[1];

    auto session = my::infer::create_session(opt);
    if (!session) {
        my::log::warn("创建会话失败: {}", session.status().to_string());
        my::log::info("提示: 不传模型路径即可用 stub 后端验证链路。");
        my::log::shutdown();
        return 0;
    }
    auto& sess = *session.value();
    my::log::info("后端: {}", sess.backend_name());

    // 输入形状以模型为准；没有模型时按 NCHW [1,3,H,W] 走
    my::infer::Shape in_shape{1, kChannels, kHeight, kWidth};
    if (!sess.inputs().empty() && sess.inputs()[0].shape.elements() > 0) {
        in_shape = sess.inputs()[0].shape;
    }

    auto output = sess.run_single_f32(tensor, in_shape);
    if (!output) {
        my::log::error("推理失败: {}", output.status().to_string());
        my::log::shutdown();
        return 1;
    }
    my::log::info("推理输出: {} 个 float", output.value().size());

    // -------------------------------------------------------------------------
    //  4) 分类后处理
    // -------------------------------------------------------------------------
    const auto top3 = my::infer::topk_softmax(output.value().data(), output.value().size(), 3);
    my::log::info("top-3 预测:");
    for (const auto& p : top3) {
        my::log::info("  class #{:<4} prob={:.4f}", p.index, p.score);
    }

    my::log::shutdown();
    return 0;
}
