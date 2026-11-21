// =============================================================================
//  示例 04：推理
//
//  用法:
//      example_infer [model.onnx]
//
//  不传参数时走 stub 后端（原样回显），用来验证调用链。
//  传入 .onnx 时优先走 onnxruntime 后端；该后端不可用的平台（如 Zynq-7000）
//  会自动回退并给出明确提示 —— 业务代码无需 #ifdef。
// =============================================================================

#include <my_cpp_libs/core/build_info.hpp>
#include <my_cpp_libs/infer/infer.hpp>
#include <my_cpp_libs/log/format.hpp>
#include <my_cpp_libs/log/log.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace {

void print_ports(const char* title, const std::vector<my::infer::Port>& ports) {
    my::log::info("{}: {} 个", title, ports.size());
    for (const auto& p : ports) {
        my::log::info("  - {:<16} type={} shape={}", p.name, my::infer::dtype_name(p.type),
                      p.shape.to_string());
    }
}

}  // namespace

int main(int argc, char** argv) {
    my::log::Config lc;
    lc.level = my::log::Level::Info;
    (void)my::log::init(lc);

    my::log::info("编译期后端: {} | 已注册: {}", my::infer::build_backend(),
                  [&] {
                      std::string s;
                      for (const auto& n : my::infer::available_backends()) {
                          if (!s.empty()) s += ", ";
                          s += n;
                      }
                      return s;
                  }());

    // -------------------------------------------------------------------------
    //  1) 创建会话
    // -------------------------------------------------------------------------
    my::infer::Options options;
    if (argc > 1) {
        options.model_path = argv[1];
    }
    options.intra_threads = 2;

    auto session_result = my::infer::create_session(options);
    if (!session_result) {
        my::log::error("创建会话失败: {}", session_result.status().to_string());
        my::log::info("提示: 不传模型路径即可使用 stub 后端跑通调用链。");
        my::log::shutdown();
        return 1;
    }

    auto& session = *session_result.value();
    my::log::info("使用后端: {}", session.backend_name());
    print_ports("输入", session.inputs());
    print_ports("输出", session.outputs());

    // -------------------------------------------------------------------------
    //  2) 准备输入并推理
    // -------------------------------------------------------------------------
    // 真实模型请按 inputs()[0].shape 分配；这里用 1x1x4 的示意数据
    std::vector<float> input_data{1.0f, 2.0f, 3.0f, 4.0f};
    my::infer::Shape   input_shape{1, 1, 4};

    if (!session.inputs().empty()) {
        const auto& port = session.inputs()[0];
        if (port.shape.elements() > 0) {
            input_shape = port.shape;
            input_data.resize(static_cast<std::size_t>(port.shape.elements()), 1.0f);
        }
    }

    auto output = session.run_single_f32(input_data, input_shape);
    if (!output) {
        my::log::error("推理失败: {}", output.status().to_string());
        my::log::shutdown();
        return 1;
    }

    my::log::info("输出元素个数: {}", output.value().size());
    std::string head;
    for (std::size_t i = 0; i < output.value().size() && i < 8; ++i) {
        head += my::log::format("{:.4f} ", output.value()[i]);
    }
    my::log::info("前几个值: {}", head);

    // -------------------------------------------------------------------------
    //  3) 显式多后端探测（换 ncnn 等后端时业务代码不变）
    // -------------------------------------------------------------------------
    for (const auto& name : my::infer::available_backends()) {
        my::log::info("后端 {} 可用 = {}", name, my::infer::has_backend(name));
    }

    my::log::shutdown();
    return 0;
}
