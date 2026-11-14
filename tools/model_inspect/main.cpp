// =============================================================================
//  model_inspect —— 模型体检工具
//
//  拿到一个陌生模型时，先跑它：
//    * 能不能加载？输入输出长什么样？动态维是什么？
//    * 用造出来的数据跑一次，输出是否有 NaN、量级是否正常？
//    * 单次推理耗时多少（x86 上测，用来估板端可行性）
//
//  用法：
//    model_inspect <model.onnx>
//    model_inspect <model.onnx> --run
//    model_inspect <model.onnx> --run --bench 20
// =============================================================================

#include <my_cpp_libs/core/build_info.hpp>
#include <my_cpp_libs/core/half.hpp>
#include <my_cpp_libs/infer/infer.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Argv {
    std::string        model;
    std::string        backend;
    bool               run   = false;
    int                bench = 0;
    std::string        dump;      // 把输出写到文件，便于跨后端比对
    int                threads = 1;
    std::vector<std::vector<std::int64_t>> shape_override;  // 可重复多次，按输入顺序对应
};

void print_usage(const char* exe) {
    std::printf(
        "用法: %s <model> [--backend NAME] [--run] [--bench N] [--shape D0,D1,...]\n"
        "  --backend NAME   强制指定后端（onnxruntime / ncnn / stub）\n"
        "  --run            用造出来的数据跑一次\n"
        "  --bench N        跑 N 次，报告平均耗时\n"
        "  --shape 1,3,224,224  覆盖第一个输入的形状（用于动态维模型）\n"
        "  --dump FILE      把输出张量写成文本（每个值一行），用于比对两个后端\n"
        "  --threads N      推理线程数（默认 1；Cortex-A9 是双核，试 2 看收益）\n",
        exe);
}

bool parse_shape(const char* text, std::vector<std::int64_t>& out) {
    std::string s(text);
    std::size_t pos = 0;
    while (pos <= s.size()) {
        const std::size_t comma = s.find(',', pos);
        const std::string tok = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (tok.empty()) return false;
        try {
            out.push_back(std::stoll(tok));
        } catch (...) {
            return false;
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return !out.empty();
}

const char* dtype_desc(my::infer::DType t) { return my::infer::dtype_name(t); }

/// 按类型造一块"温和"的数据：全 1 容易看不出问题，这里用 0.5 附近的小值
std::vector<std::uint8_t> make_input(const my::infer::Tensor& t) {
    std::vector<std::uint8_t> buf(t.byte_size(), 0);
    const std::size_t count = t.byte_size() / std::max<std::size_t>(1, my::infer::dtype_size(t.type));
    for (std::size_t i = 0; i < count; ++i) {
        const float v = 0.5f;
        switch (t.type) {
            case my::infer::DType::Float32: {
                const float x = v;
                std::memcpy(buf.data() + i * 4, &x, 4);
                break;
            }
            case my::infer::DType::Float64: {
                const double x = v;
                std::memcpy(buf.data() + i * 8, &x, 8);
                break;
            }
            case my::infer::DType::Float16: {
                const std::uint16_t x = my::float_to_half(v);
                std::memcpy(buf.data() + i * 2, &x, 2);
                break;
            }
            case my::infer::DType::Int8: {
                const std::int8_t x = 1;
                std::memcpy(buf.data() + i, &x, 1);
                break;
            }
            case my::infer::DType::UInt8:
            case my::infer::DType::Bool: {
                buf[i] = 1;
                break;
            }
            case my::infer::DType::Int32: {
                const std::int32_t x = 1;
                std::memcpy(buf.data() + i * 4, &x, 4);
                break;
            }
            case my::infer::DType::Int64: {
                const std::int64_t x = 1;
                std::memcpy(buf.data() + i * 8, &x, 8);
                break;
            }
            default:
                break;
        }
    }
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    Argv args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--run") {
            args.run = true;
        } else if (a == "--bench" && i + 1 < argc) {
            args.bench = std::atoi(argv[++i]);
        } else if (a == "--threads" && i + 1 < argc) {
            args.threads = std::atoi(argv[++i]);
        } else if (a == "--dump" && i + 1 < argc) {
            args.dump = argv[++i];
        } else if (a == "--backend" && i + 1 < argc) {
            args.backend = argv[++i];
        } else if (a == "--shape" && i + 1 < argc) {
            std::vector<std::int64_t> one;
            if (!parse_shape(argv[++i], one)) {
                std::printf("--shape 参数格式不对\n");
                return 2;
            }
            args.shape_override.push_back(std::move(one));
        } else if (a == "-h" || a == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (!a.empty() && a[0] == '-') {
            print_usage(argv[0]);
            return 2;
        } else {
            args.model = a;
        }
    }

    if (args.model.empty()) {
        print_usage(argv[0]);
        return 2;
    }

    std::printf("=== my_cpp_libs model_inspect ===\n");
    std::printf("%s\n\n", my::BuildInfo::summary());

    my::infer::Options opt;
    opt.model_path    = args.model;
    opt.backend       = args.backend;
    opt.intra_threads = args.threads;

    std::printf("[1] 加载模型: %s\n", args.model.c_str());
    auto info = my::infer::inspect_model(opt);
    if (!info) {
        std::printf("    失败: %s\n", info.status().to_string().c_str());
        return 1;
    }

    const auto& m = info.value();
    std::printf("    后端: %s\n", m.backend.c_str());

    std::printf("\n[2] 输入 (%zu)\n", m.inputs.size());
    for (const auto& p : m.inputs) {
        std::printf("    %-20s %-6s %s\n", p.name.c_str(), dtype_desc(p.type), p.shape.to_string().c_str());
    }
    std::printf("\n[3] 输出 (%zu)\n", m.outputs.size());
    for (const auto& p : m.outputs) {
        std::printf("    %-20s %-6s %s\n", p.name.c_str(), dtype_desc(p.type), p.shape.to_string().c_str());
    }

    if (!args.run && args.bench == 0) {
        std::printf("\n（加 --run 试跑一次，--bench N 测延迟）\n");
        return 0;
    }

    // -------------------------------------------------------------------------
    //  造数据
    // -------------------------------------------------------------------------
    auto session = my::infer::create_session(opt);
    if (!session) {
        std::printf("\n创建会话失败: %s\n", session.status().to_string().c_str());
        return 1;
    }
    auto& sess = *session.value();

    std::vector<std::vector<std::uint8_t>> buffers;
    std::vector<my::infer::Feed>           feeds;

    buffers.reserve(sess.inputs().size());
    for (std::size_t i = 0; i < sess.inputs().size(); ++i) {
        const auto& port = sess.inputs()[i];

        my::infer::Shape shape = port.shape;
        if (!args.shape_override.empty()) {
            shape = my::infer::Shape(i < args.shape_override.size() ? args.shape_override[i]
                                                                  : args.shape_override.back());
        }
        for (auto& d : shape.dims) {
            if (d <= 0) d = 1;  // 动态维用 1 占位，保证能算缓冲区
        }
        if (port.type == my::infer::DType::Unknown) {
            std::printf("\n输入 %s 类型未知，无法自动造数据\n", port.name.c_str());
            return 1;
        }
        if (shape.dims.empty()) {
            std::printf("\n输入 %s 没有形状信息（ncnn 这类后端运行期才知道），请用 --shape 指定\n",
                        port.name.c_str());
            return 1;
        }

        my::infer::Tensor t(port.type, shape, nullptr);
        buffers.push_back(make_input(t));
        t.data  = buffers.back().data();
        t.bytes = buffers.back().size();
        feeds.push_back(my::infer::Feed{port.name, t});
    }

    std::printf("\n[4] 试跑（输入数据都是 0.5/1 这类温和值）\n");
    std::vector<my::infer::Outcome> outcomes;
    auto st = sess.run(feeds, outcomes);
    if (!st) {
        std::printf("    推理失败: %s\n", st.to_string().c_str());
        return 1;
    }

    for (const auto& o : outcomes) {
        auto floats = my::infer::tensor_to_float32(o.tensor);
        if (!floats) {
            std::printf("    %-16s %s （转 float 失败: %s）\n", o.name.c_str(),
                        my::infer::tensor_to_string(o.tensor).c_str(),
                        floats.status().to_string().c_str());
            continue;
        }
        const auto& v = floats.value();
        double mn = v.empty() ? 0.0 : v[0];
        double mx = mn;
        double sum = 0.0;
        int    nan = 0;
        for (float x : v) {
            if (!std::isfinite(x)) {
                ++nan;
                continue;
            }
            mn = std::min<double>(mn, x);
            mx = std::max<double>(mx, x);
            sum += x;
        }
        std::printf("    %-16s %s\n", o.name.c_str(), my::infer::tensor_to_string(o.tensor).c_str());
        std::printf("      min=%.6g max=%.6g mean=%.6g", mn, mx, v.empty() ? 0.0 : sum / (double)v.size());
        if (nan) std::printf("  [含 %d 个 NaN/Inf]", nan);
        std::printf("\n      前几个: ");
        for (std::size_t k = 0; k < v.size() && k < 8; ++k) std::printf("%.4f ", v[k]);
        std::printf("\n");
    }

    // -------------------------------------------------------------------------
    //  导出（用于跨后端数值比对）
    // -------------------------------------------------------------------------
    if (!args.dump.empty()) {
        std::FILE* f = std::fopen(args.dump.c_str(), "wb");
        if (f == nullptr) {
            std::printf("    无法写入 %s\n", args.dump.c_str());
        } else {
            for (const auto& o : outcomes) {
                auto fl = my::infer::tensor_to_float32(o.tensor);
                if (!fl) continue;
                std::fprintf(f, "# %s %s\n", o.name.c_str(),
                             o.tensor.shape.to_string().c_str());
                // %.9g：float 的完整精度，保证比对时不受格式化损失影响
                for (float v : fl.value()) std::fprintf(f, "%.9g\n", v);
            }
            std::fclose(f);
            std::printf("    输出已写入 %s\n", args.dump.c_str());
        }
    }

    // -------------------------------------------------------------------------
    //  延迟
    // -------------------------------------------------------------------------
    if (args.bench > 0) {
        std::printf("\n[5] 延迟（%d 次）\n", args.bench);
        // 预热一次
        sess.run(feeds, outcomes);

        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < args.bench; ++i) {
            auto s = sess.run(feeds, outcomes);
            if (!s) {
                std::printf("    第 %d 次失败: %s\n", i, s.to_string().c_str());
                break;
            }
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count() / static_cast<double>(args.bench);
        std::printf("    平均 %.3f ms/次\n", ms);
    }

    return 0;
}
