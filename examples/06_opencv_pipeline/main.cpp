// =============================================================================
//  示例 06：OpenCV + 推理（C1 阶段的完整链路）
//
//  读图/造图 -> letterbox 预处理 -> 推理 -> 分类后处理 -> 把结果画回原图存档
//
//  用法：
//      example_opencv [图像路径] [模型路径]
//  都不传时：自动造一张图 + 用 models/small_cnn.onnx（32x32 输入）
// =============================================================================

#include <my_cpp_libs/core/build_info.hpp>
#include <my_cpp_libs/cv/image.hpp>
#include <my_cpp_libs/infer/infer.hpp>
#include <my_cpp_libs/infer/preprocess.hpp>
#include <my_cpp_libs/log/format.hpp>
#include <my_cpp_libs/log/log.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {

/// 没有输入图片时造一张：左边红、右边蓝、中间有渐变
my::cv::Image make_demo_image(int width, int height) {
    my::cv::Image img(height, width, CV_8UC3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double u = static_cast<double>(x) / width;
            const double v = static_cast<double>(y) / height;
            img.at<cv::Vec3b>(y, x) = cv::Vec3b(
                static_cast<unsigned char>((1.0 - u) * 255),   // B
                static_cast<unsigned char>(v * 255),           // G
                static_cast<unsigned char>(u * 255));          // R
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
    my::log::info("OpenCV {}", CV_VERSION);

    std::error_code ec;
    std::filesystem::create_directories("output", ec);

    // =========================================================================
    //  1) 拿到一张图
    // =========================================================================
    my::cv::Image image;
    if (argc > 1) {
        auto loaded = my::cv::load(argv[1]);
        if (!loaded) {
            my::log::error("读图失败: {}", loaded.status().to_string());
            return 1;
        }
        image = loaded.value();
    } else {
        image = make_demo_image(160, 120);
        my::log::info("没有指定图片，已合成 {}x{} 的演示图", image.cols, image.rows);
    }
    my::log::info("输入图像: {}x{}, {} 通道", image.cols, image.rows, image.channels());

    // =========================================================================
    //  2) 预处理：letterbox + 颜色转换 + 归一化 + HWC->NCHW
    // =========================================================================
    my::cv::PreprocessSpec spec;
    spec.input_width  = 32;   // small_cnn 的输入尺寸
    spec.input_height = 32;
    spec.keep_aspect  = true;

    spec.norm.src_order = my::infer::ColorOrder::BGR;  // cv::imread 读出来是 BGR
    spec.norm.dst_order = my::infer::ColorOrder::RGB;
    spec.norm.layout    = my::infer::Layout::NCHW;
    spec.norm.scale     = 1.0f / 255.0f;

    auto prepared = my::cv::prepare(image, spec);
    if (!prepared) {
        my::log::error("预处理失败: {}", prepared.status().to_string());
        return 1;
    }
    const auto& prep = prepared.value();
    my::log::info("letterbox: scale={:.4f} pad=({}, {})", prep.letterbox.scale, prep.letterbox.pad_x,
                  prep.letterbox.pad_y);
    my::log::info("张量: {} 个 float, 建议形状 {}",
                  prep.tensor.size(), prep.shape.to_string());

    if (auto st = my::cv::save("output/cv_preprocessed.png", prep.image); !st) {
        my::log::warn("保存预处理图失败: {}", st.to_string());
    }

    // =========================================================================
    //  3) 推理
    // =========================================================================
    my::infer::Options opt;
    if (argc > 2) {
        opt.model_path = argv[2];
    } else if (std::filesystem::exists("models/small_cnn.onnx")) {
        opt.model_path = "models/small_cnn.onnx";
    }

    auto session = my::infer::create_session(opt);
    if (!session) {
        my::log::warn("创建会话失败: {}", session.status().to_string());
        my::log::info("（没有模型也能验证预处理链路，上面已经跑完了）");
        my::log::shutdown();
        return 0;
    }
    auto& sess = *session.value();
    my::log::info("后端: {}", sess.backend_name());

    auto output = sess.run_single_f32(prep.tensor, prep.shape);
    if (!output) {
        my::log::error("推理失败: {}", output.status().to_string());
        my::log::shutdown();
        return 1;
    }

    // =========================================================================
    //  4) 后处理
    // =========================================================================
    const auto top3 = my::infer::topk_softmax(output.value().data(), output.value().size(), 3);
    my::log::info("top-3:");
    for (const auto& p : top3) {
        my::log::info("  class #{:<3} prob={:.4f}", p.index, p.score);
    }

    // =========================================================================
    //  5) 结果画回原图
    // =========================================================================
    my::cv::Image annotated = image.clone();
    if (!top3.empty()) {
        const std::string text =
            my::log::format("class {} ({:.2f})", top3[0].index, top3[0].score);
        cv::putText(annotated, text, cv::Point(6, 22), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                    cv::Scalar(0, 255, 0), 1, cv::LINE_AA);
    }
    // 画一下 letterbox 映射：把输入图中心点映射回原图坐标
    {
        const float cx = 16.0f, cy = 16.0f;
        float       x1 = cx, y1 = cy, x2 = cx, y2 = cy;
        my::infer::unletterbox_box(x1, y1, x2, y2, prep.letterbox);
        cv::circle(annotated, cv::Point(static_cast<int>(x1), static_cast<int>(y1)), 4,
                   cv::Scalar(0, 0, 255), -1);
        my::log::info("输入图中心 (16,16) 映射回原图 = ({:.1f}, {:.1f})", x1, y1);
    }

    if (auto st = my::cv::save("output/cv_result.png", annotated); !st) {
        my::log::error("保存结果失败: {}", st.to_string());
    } else {
        my::log::info("结果已保存: output/cv_result.png");
    }

    my::log::shutdown();
    return 0;
}
