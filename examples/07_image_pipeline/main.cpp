// =============================================================================
//  example_image_pipeline —— 板端图像链路（不需要 OpenCV）
//
//  演示的正是 Zynq 上跑的那条链：
//      合成/解码一张图 -> letterbox 预处理 -> 推理 -> 把框映射回原图
//
//  和 06_opencv_pipeline 的区别：这里一行 OpenCV 都没有，
//  解码/编码走 stb（my::image），缩放/归一化也全在库里完成。
//  所以这个示例在 armv7 上同样能编、能跑。
//
//  没有推理库时（MinGW / 没装 ORT）会自动落到 stub 后端，
//  它把输入原样回显 —— 链路照样能验证，只是"推理"那一步是假的。
// =============================================================================

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "my_cpp_libs/image/image.hpp"
#include "my_cpp_libs/infer/infer.hpp"
#include "my_cpp_libs/log/format.hpp"  // my::log::info(...)：{} 格式化在这一层
#include "my_cpp_libs/log/log.hpp"

namespace {

constexpr int kFrameWidth  = 640;
constexpr int kFrameHeight = 480;

/// 造一张假相机帧：横向青-蓝渐变 + 中间一块红色矩形（模拟"待检测目标"）
my::image::Image make_frame() {
    my::image::Image frame;
    frame.width    = kFrameWidth;
    frame.height   = kFrameHeight;
    frame.channels = 3;
    frame.pixels.resize(frame.byte_size());

    for (int y = 0; y < kFrameHeight; ++y) {
        for (int x = 0; x < kFrameWidth; ++x) {
            std::uint8_t* p = frame.pixels.data() +
                              (static_cast<std::size_t>(y) * kFrameWidth + x) * 3;
            p[0] = static_cast<std::uint8_t>(x * 255 / (kFrameWidth - 1));   // R
            p[1] = static_cast<std::uint8_t>(y * 255 / (kFrameHeight - 1));  // G
            p[2] = static_cast<std::uint8_t>(x * 255 / (kFrameWidth - 1));   // B
        }
    }

    const int x0 = 200, y0 = 140, x1 = 440, y1 = 340;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            std::uint8_t* p = frame.pixels.data() +
                              (static_cast<std::size_t>(y) * kFrameWidth + x) * 3;
            p[0] = 220;
            p[1] = 40;
            p[2] = 40;
        }
    }
    return frame;
}

std::vector<std::uint8_t> read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    my::log::Config cfg;
    cfg.level = my::log::Level::Info;
    cfg.to_console = true;
    my::log::init(cfg);

    // -------------------------------------------------------------------------
    // 1. 编码 -> 落盘 -> 读回来。板端相机给的是内存帧，SD 卡上的图走文件，
    //    两条路径都要能走通，所以这里两个都演示。
    // -------------------------------------------------------------------------
    const std::string frame_path = "image_pipeline_frame.png";
    const my::image::Image frame = make_frame();
    my::log::info("合成帧: {}", frame.to_string());

    if (auto st = my::image::save(frame_path, frame); !st) {
        my::log::error("保存 PNG 失败: {}", st.to_string());
        return 1;
    }

    auto from_file = my::image::load(frame_path, /*channels=*/3);
    if (!from_file) {
        my::log::error("读图失败: {}", from_file.status().to_string());
        return 1;
    }
    my::log::info("文件解码: {}", from_file.value().to_string());

    const std::vector<std::uint8_t> bytes = read_all(frame_path);
    auto from_memory = my::image::load_memory(bytes.data(), bytes.size(), /*channels=*/3);
    if (!from_memory) {
        my::log::error("内存解码失败: {}", from_memory.status().to_string());
        return 1;
    }
    my::log::info("内存解码: {}（{} 字节）", from_memory.value().to_string(), bytes.size());

    // -------------------------------------------------------------------------
    // 2. 预处理：640x480 -> 640x640 letterbox。
    //    注意 src_order 默认就是 RGB（stb 解码出来是 RGB），不需要改。
    // -------------------------------------------------------------------------
    my::image::PreprocessSpec spec;
    spec.input_width  = 640;
    spec.input_height = 640;
    spec.keep_aspect  = true;
    spec.pad_value    = 114;
    spec.norm.scale   = 1.0f / 255.0f;
    spec.norm.dst_order = my::infer::ColorOrder::RGB;
    spec.norm.mean      = {0.485f, 0.456f, 0.406f};
    spec.norm.stddev    = {0.229f, 0.224f, 0.225f};

    auto prep = my::image::prepare(from_memory.value(), spec);
    if (!prep) {
        my::log::error("预处理失败: {}", prep.status().to_string());
        return 1;
    }
    my::log::info("张量: shape={} 元素={} 通道={}", prep.value().shape.to_string(),
                  prep.value().tensor.size(), prep.value().channels);
    my::log::info("letterbox: scale=({}, {}) pad=({}, {})", prep.value().letterbox.scale_x,
                  prep.value().letterbox.scale_y, prep.value().letterbox.pad_x,
                  prep.value().letterbox.pad_y);

    // 把 letterbox 之后的画布存一张，肉眼确认 padding 是 114 灰边
    if (auto st = my::image::save("image_pipeline_canvas.png", prep.value().canvas); !st) {
        my::log::warn("保存画布失败: {}", st.to_string());
    }

    // -------------------------------------------------------------------------
    // 3. 推理。没有模型/没有后端时会落到 stub，输入原样回显。
    // -------------------------------------------------------------------------
    my::infer::Options opt;  // 不给 model_path => 自动选 stub
    opt.intra_threads = 2;

    auto session = my::infer::create_session(opt);
    if (!session) {
        my::log::error("创建会话失败: {}", session.status().to_string());
        return 1;
    }
    my::log::info("推理后端: {}", session.value()->backend_name());

    auto out = session.value()->run_single_f32(prep.value().tensor, prep.value().shape);
    if (!out) {
        my::log::error("推理失败: {}", out.status().to_string());
        return 1;
    }
    my::log::info("输出元素数: {}", out.value().size());

    // -------------------------------------------------------------------------
    // 4. 后处理：模型坐标系下的框 -> 原图坐标系。
    //    这是最容易写错的一步（padding 和缩放算反了，框会整体偏移）。
    // -------------------------------------------------------------------------
    // 假一个"模型输出"：目标矩形经过 letterbox 之后落在模型空间的位置。
    float box[4] = {
        200.0f * prep.value().letterbox.scale_x + prep.value().letterbox.pad_x,
        140.0f * prep.value().letterbox.scale_y + prep.value().letterbox.pad_y,
        440.0f * prep.value().letterbox.scale_x + prep.value().letterbox.pad_x,
        340.0f * prep.value().letterbox.scale_y + prep.value().letterbox.pad_y,
    };
    my::log::info("模型空间框: ({:.1f}, {:.1f}) - ({:.1f}, {:.1f})", box[0], box[1], box[2], box[3]);

    my::image::unletterbox_boxes(box, 1, prep.value().letterbox);
    my::log::info("映射回原图: ({:.1f}, {:.1f}) - ({:.1f}, {:.1f})  期望 (200, 140) - (440, 340)",
                  box[0], box[1], box[2], box[3]);

    my::log::info("产物: {} / image_pipeline_canvas.png", frame_path);
    my::log::shutdown();
    return 0;
}
