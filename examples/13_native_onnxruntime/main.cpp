// =============================================================================
//  示例 13：ONNX Runtime 原生 C++ API
//
//  用途：对照 my::infer 封装前后的差异。这里直接用 Ort:: 命名空间。
//  用法：example_native_onnxruntime <model.onnx>
// =============================================================================

#include <onnxruntime_cxx_api.h>

#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace {

#if defined(_WIN32)
std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int len =
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), len);
    return out;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("用法: %s <model.onnx>\n", argv[0]);
        std::printf("提示: 没有现成模型时，可先用封装层 example_infer（stub 后端）跑通流程。\n");
        return 2;
    }
    const std::string model_path = argv[1];

    try {
        // ---------------------------------------------------------------------
        //  1) 环境：整个进程一个即可
        // ---------------------------------------------------------------------
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "native_demo");

        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        // ---------------------------------------------------------------------
        //  2) 载入模型（Windows 下路径是 wchar_t*）
        // ---------------------------------------------------------------------
#if defined(_WIN32)
        const std::wstring wide_path = widen(model_path);
        Ort::Session session(env, wide_path.c_str(), options);
#else
        Ort::Session session(env, model_path.c_str(), options);
#endif

        Ort::AllocatorWithDefaultOptions allocator;

        // ---------------------------------------------------------------------
        //  3) 查输入输出
        // ---------------------------------------------------------------------
        const std::size_t n_inputs  = session.GetInputCount();
        const std::size_t n_outputs = session.GetOutputCount();

        std::vector<std::string>         input_names;
        std::vector<std::vector<int64_t>> input_shapes;
        for (std::size_t i = 0; i < n_inputs; ++i) {
            auto name = session.GetInputNameAllocated(i, allocator);
            input_names.emplace_back(name.get());

            auto info  = session.GetInputTypeInfo(i);
            auto shape = info.GetTensorTypeAndShapeInfo().GetShape();
            input_shapes.push_back(shape);

            std::printf("输入[%zu] %s shape=[", i, input_names.back().c_str());
            for (std::size_t k = 0; k < shape.size(); ++k) std::printf("%s%lld", k ? "," : "",
                                                                      static_cast<long long>(shape[k]));
            std::printf("]\n");
        }

        std::vector<std::string> output_names;
        for (std::size_t i = 0; i < n_outputs; ++i) {
            auto name = session.GetOutputNameAllocated(i, allocator);
            output_names.emplace_back(name.get());
            std::printf("输出[%zu] %s\n", i, output_names.back().c_str());
        }

        // ---------------------------------------------------------------------
        //  4) 造一个全 1 输入并推理
        // ---------------------------------------------------------------------
        if (n_inputs != 1) {
            std::printf("本示例只处理单输入模型\n");
            return 0;
        }

        std::size_t count = 1;
        for (auto d : input_shapes[0]) {
            count *= static_cast<std::size_t>(d > 0 ? d : 1);
        }
        std::vector<float> input_data(count, 1.0f);

        auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<Ort::Value> input_values;
        input_values.push_back(Ort::Value::CreateTensor<float>(
            memory_info, input_data.data(), input_data.size(), input_shapes[0].data(),
            input_shapes[0].size()));

        std::vector<const char*> in_names;
        for (auto& n : input_names) in_names.push_back(n.c_str());
        std::vector<const char*> out_names;
        for (auto& n : output_names) out_names.push_back(n.c_str());

        auto outputs = session.Run(Ort::RunOptions{nullptr}, in_names.data(), input_values.data(),
                                   input_values.size(), out_names.data(), out_names.size());

        // ---------------------------------------------------------------------
        //  5) 读结果
        // ---------------------------------------------------------------------
        for (std::size_t i = 0; i < outputs.size(); ++i) {
            auto info  = outputs[i].GetTensorTypeAndShapeInfo();
            auto shape = info.GetShape();
            const std::size_t n = info.GetElementCount();
            const float* p = outputs[i].GetTensorMutableData<float>();

            std::printf("输出[%zu] 元素数=%zu 前几个值: ", i, n);
            for (std::size_t k = 0; k < n && k < 8; ++k) std::printf("%.4f ", p[k]);
            std::printf("\n");
        }
    } catch (const Ort::Exception& e) {
        std::printf("ONNX Runtime 异常: %s (code=%d)\n", e.what(), e.GetOrtErrorCode());
        return 1;
    } catch (const std::exception& e) {
        std::printf("异常: %s\n", e.what());
        return 1;
    }

    return 0;
}
