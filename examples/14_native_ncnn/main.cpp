// =============================================================================
//  示例 14：ncnn 原生 API
//
//  对照 my::infer 的 ncnn 后端，看封装前后差多少。
//
//  用法：
//      example_native_ncnn <model.param> [形状]
//      # 形状写成 1,3,32,32 或 C,H,W；省略时按一维 8 处理
//
//  例：
//      example_native_ncnn models\ncnn\small_cnn.param 1,3,32,32
// =============================================================================

#include <ncnn/net.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

/// "1,3,32,32" -> w/h/c（ncnn 没有 batch 维）
void parse_shape(const char* text, int& w, int& h, int& c) {
    std::vector<int> dims;
    const char*      p = text;
    while (*p) {
        char*       end = nullptr;
        const long  v   = std::strtol(p, &end, 10);
        if (end == p) break;
        dims.push_back(static_cast<int>(v));
        p = (*end == ',') ? end + 1 : end;
    }

    if (dims.empty()) {
        w = 8;
        h = 1;
        c = 1;
    } else if (dims.size() == 1) {
        w = dims[0];
        h = 1;
        c = 1;
    } else if (dims.size() == 2) {
        w = dims[1];
        h = 1;
        c = dims[0];
    } else if (dims.size() == 3) {
        w = dims[2];
        h = dims[1];
        c = dims[0];
    } else {  // 4 维：[N,C,H,W]，N 必须为 1
        w = dims[3];
        h = dims[2];
        c = dims[1];
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("用法: %s <model.param> [形状 如 1,3,32,32]\n", argv[0]);
        std::printf("提示: python tools/gen_ncnn_test_model.py models/ncnn\n");
        return 2;
    }

    const std::string param_path = argv[1];
    std::string       bin_path   = param_path;
    const auto        dot        = bin_path.rfind(".param");
    if (dot != std::string::npos) {
        bin_path = bin_path.substr(0, dot) + ".bin";
    }

    int w = 8, h = 1, c = 1;
    if (argc > 2) parse_shape(argv[2], w, h, c);

    // -------------------------------------------------------------------------
    //  1) 加载模型
    // -------------------------------------------------------------------------
    ncnn::Net net;
    net.opt.use_vulkan_compute  = false;
    net.opt.num_threads         = 2;
    net.opt.use_fp16_packed     = false;
    net.opt.use_fp16_storage    = false;
    net.opt.use_fp16_arithmetic = false;

    if (net.load_param(param_path.c_str()) != 0) {
        std::printf("load_param 失败: %s\n", param_path.c_str());
        return 1;
    }
    if (net.load_model(bin_path.c_str()) != 0) {
        std::printf("load_model 失败: %s\n", bin_path.c_str());
        return 1;
    }

    std::printf("输入 blob:");
    for (const char* n : net.input_names()) std::printf(" %s", n);
    std::printf("\n输出 blob:");
    for (const char* n : net.output_names()) std::printf(" %s", n);
    std::printf("\n");

    // -------------------------------------------------------------------------
    //  2) 造输入：全部 0.5（和 model_inspect 的填充值一致，便于比对）
    // -------------------------------------------------------------------------
    ncnn::Mat in(w, h, c);
    if (in.empty()) {
        std::printf("分配输入失败 %dx%dx%d\n", w, h, c);
        return 1;
    }
    for (int i = 0; i < w * h * c; ++i) in[i] = 0.5f;

    // -------------------------------------------------------------------------
    //  3) 推理
    // -------------------------------------------------------------------------
    // 线程数要在 load_param 之前设到 net.opt 上；Extractor::set_num_threads 已经是 no-op
    ncnn::Extractor ex = net.create_extractor();
    if (ex.input(net.input_names()[0], in) != 0) {
        std::printf("设置输入失败\n");
        return 1;
    }

    ncnn::Mat out;
    if (ex.extract(net.output_names()[0], out) != 0) {
        std::printf("取输出失败\n");
        return 1;
    }

    // 输出可能是打包布局，统一拆成 elempack=1 再读
    ncnn::Mat plain;
    if (out.elempack != 1) {
        ncnn::convert_packing(out, plain, 1);
    } else {
        plain = out;
    }

    std::printf("输出 dims=%d w=%d h=%d c=%d\n", plain.dims, plain.w, plain.h, plain.c);
    std::printf("前几个值: ");
    const int n = plain.w * plain.h * plain.c;
    for (int i = 0; i < n && i < 10; ++i) std::printf("%.6f ", plain[i]);
    std::printf("\n");

    return 0;
}
