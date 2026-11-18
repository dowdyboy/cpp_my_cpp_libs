// =============================================================================
//  ONNX Runtime 后端
//
//  仅在编译期检测到可链接的 ORT 时参与编译（见 cmake/Deps.cmake）。
//  所有 Ort::* 类型都被关在本文件内，公共头不出现 ORT 符号。
// =============================================================================

#include "my_cpp_libs/infer/infer.hpp"

#include "backends.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#if defined(MYLIB_HAVE_ONNXRUNTIME)

#include <onnxruntime_cxx_api.h>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace my::infer {
namespace {

Ort::Env& ort_env() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "my_cpp_libs");
    return env;
}

#if defined(_WIN32)
std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int len = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                                          nullptr, 0);
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), len);
    return out;
}
#endif

DType from_ort(ONNXTensorElementDataType t) noexcept {
    switch (t) {
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT: return DType::Float32;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16: return DType::Float16;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE: return DType::Float64;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8: return DType::Int8;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8: return DType::UInt8;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32: return DType::Int32;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64: return DType::Int64;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL: return DType::Bool;
        default: return DType::Unknown;
    }
}

bool to_ort(DType t, ONNXTensorElementDataType& out) noexcept {
    switch (t) {
        // fp16 不需要在 C++ 侧解包：ORT 的 CreateTensorWithDataAsOrtValue 收的是
        // "裸字节 + 元素类型"，只要字节数对得上就能直接喂。
        case DType::Float16: out = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16; return true;
        case DType::Float32: out = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT; return true;
        case DType::Float64: out = ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE; return true;
        case DType::Int8: out = ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8; return true;
        case DType::UInt8: out = ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8; return true;
        case DType::Int32: out = ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32; return true;
        case DType::Int64: out = ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64; return true;
        case DType::Bool: out = ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL; return true;
        default: return false;
    }
}

// -----------------------------------------------------------------------------
class OrtxSession final : public Session {
public:
    explicit OrtxSession(const Options& options) {
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(std::max(1, options.intra_threads));
        so.SetInterOpNumThreads(std::max(1, options.inter_threads));
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        so.SetLogSeverityLevel(options.verbose ? 1 : 3);
        for (const auto& kv : options.provider_options) {
            so.AddConfigEntry(kv.first.c_str(), kv.second.c_str());
        }

#if defined(_WIN32)
        const std::wstring model_path = widen(options.model_path);
        session_.reset(new Ort::Session(ort_env(), model_path.c_str(), so));
#else
        session_.reset(new Ort::Session(ort_env(), options.model_path.c_str(), so));
#endif

        Ort::AllocatorWithDefaultOptions allocator;

        const std::size_t n_in = session_->GetInputCount();
        for (std::size_t i = 0; i < n_in; ++i) {
            auto        owned_name = session_->GetInputNameAllocated(i, allocator);
            std::string name_str   = owned_name.get();
            Port        p;
            p.name  = name_str;
            p.type  = read_type(i, /*output=*/false);
            p.shape = read_shape(i, /*output=*/false);
            input_names_.push_back(std::move(name_str));
            input_ports_.push_back(std::move(p));
        }

        const std::size_t n_out = session_->GetOutputCount();
        for (std::size_t i = 0; i < n_out; ++i) {
            auto        owned_name = session_->GetOutputNameAllocated(i, allocator);
            std::string name_str   = owned_name.get();
            Port        p;
            p.name  = name_str;
            p.type  = read_type(i, /*output=*/true);
            p.shape = read_shape(i, /*output=*/true);
            output_names_.push_back(std::move(name_str));
            output_ports_.push_back(std::move(p));
        }
    }

    const char* backend_name() const noexcept override { return "onnxruntime"; }

    const std::vector<Port>& inputs() const noexcept override { return input_ports_; }
    const std::vector<Port>& outputs() const noexcept override { return output_ports_; }

    Status run(const std::vector<Feed>& feeds, std::vector<Outcome>& outcomes) override {
        try {
            std::vector<Ort::Value> input_values;
            input_values.reserve(input_ports_.size());

            for (const auto& port : input_ports_) {
                const Tensor* tensor = nullptr;
                for (const auto& feed : feeds) {
                    if (feed.name == port.name) {
                        tensor = &feed.tensor;
                        break;
                    }
                }
                if (tensor == nullptr) {
                    if (feeds.size() == 1 && input_ports_.size() == 1) {
                        tensor = &feeds[0].tensor;  // 单入单出时允许不写名字
                    } else {
                        return Status(Errc::InvalidArgument, "缺少输入: " + port.name);
                    }
                }

                ONNXTensorElementDataType ort_type{};
                if (!to_ort(tensor->type, ort_type)) {
                    return Status(Errc::Unsupported,
                                  std::string("暂不支持的输入类型: ") + dtype_name(tensor->type));
                }

                std::vector<std::int64_t> dims(tensor->shape.dims.begin(), tensor->shape.dims.end());
                if (dims.empty()) {
                    return Status(Errc::InvalidArgument, "输入形状不能为空: " + port.name);
                }
                for (auto& d : dims) {
                    if (d <= 0) {
                        // 模型端口可以是动态维（-1），但真正喂数据时必须给出确定形状，
                        // 否则没法算缓冲区大小。这里明确报错，而不是偷偷改成 1。
                        return Status(Errc::InvalidArgument,
                                      "输入 '" + port.name +
                                          "' 的形状含动态维，喂数据时必须给出具体形状");
                    }
                }

                input_values.push_back(Ort::Value::CreateTensor(
                    memory_info(), tensor->data, tensor->byte_size(), dims.data(), dims.size(),
                    ort_type));
            }

            std::vector<const char*> out_names_ptr;
            out_names_ptr.reserve(output_names_.size());
            for (const auto& n : output_names_) out_names_ptr.push_back(n.c_str());

            std::vector<const char*> in_names_ptr;
            in_names_ptr.reserve(input_names_.size());
            for (const auto& n : input_names_) in_names_ptr.push_back(n.c_str());

            last_outputs_ = session_->Run(Ort::RunOptions{nullptr}, in_names_ptr.data(),
                                          input_values.data(), input_values.size(),
                                          out_names_ptr.data(), out_names_ptr.size());

            outcomes.clear();
            outcomes.reserve(last_outputs_.size());
            for (std::size_t i = 0; i < last_outputs_.size(); ++i) {
                auto info = last_outputs_[i].GetTensorTypeAndShapeInfo();
                auto dims = info.GetShape();

                // 字节数必须按**模型实际输出类型**算，不能信端口声明：
                // 声明和实际不一致时（真实模型里 fp16/uint8 输出很常见），
                // 下游 tensor_to_float32 会按错误的 bytes 去读，直接越界。
                ONNXTensorElementDataType declared{};
                if (!to_ort(output_ports_[i].type, declared) ||
                    info.GetElementType() != declared) {
                    return Status(Errc::Unsupported,
                                  "模型输出 '" + output_ports_[i].name +
                                      "' 的实际数据类型与端口声明不一致（算子/图被改过？）");
                }
                auto bytes = info.GetElementCount() * dtype_size(output_ports_[i].type);

                Outcome o;
                o.name          = output_ports_[i].name;
                o.tensor.type   = output_ports_[i].type;
                o.tensor.shape  = Shape(dims);
                o.tensor.data   = last_outputs_[i].GetTensorMutableData<void>();
                o.tensor.bytes  = bytes;
                outcomes.push_back(std::move(o));
            }
            return Ok();
        } catch (const Ort::Exception& e) {
            return Status(Errc::RuntimeError, std::string("ONNX Runtime: ") + e.what());
        } catch (const std::exception& e) {
            return Status(Errc::RuntimeError, std::string("推理失败: ") + e.what());
        }
    }

private:
    static OrtMemoryInfo* memory_info() {
        static Ort::MemoryInfo info =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        return info;
    }

    DType read_type(std::size_t index, bool output) const noexcept {
        try {
            auto info = output ? session_->GetOutputTypeInfo(index) : session_->GetInputTypeInfo(index);
            auto tensor_info = info.GetTensorTypeAndShapeInfo();
            return from_ort(tensor_info.GetElementType());
        } catch (...) {
            return DType::Unknown;
        }
    }

    Shape read_shape(std::size_t index, bool output) const noexcept {
        try {
            auto info = output ? session_->GetOutputTypeInfo(index) : session_->GetInputTypeInfo(index);
            auto dims = info.GetTensorTypeAndShapeInfo().GetShape();
            return Shape(dims);
        } catch (...) {
            return Shape{};
        }
    }

    std::unique_ptr<Ort::Session> session_;
    std::vector<std::string>      input_names_;
    std::vector<std::string>      output_names_;
    std::vector<Port>             input_ports_;
    std::vector<Port>             output_ports_;
    std::vector<Ort::Value>       last_outputs_;  // 输出缓冲区所有者
};

Result<std::unique_ptr<Session>> make_onnxruntime(const Options& options) {
    if (options.model_path.empty()) {
        return Status(Errc::InvalidArgument, "onnxruntime 后端需要 model_path");
    }
    try {
        return std::unique_ptr<Session>(new OrtxSession(options));
    } catch (const Ort::Exception& e) {
        return Status(Errc::RuntimeError, std::string("加载 ONNX 模型失败: ") + e.what());
    } catch (const std::exception& e) {
        return Status(Errc::RuntimeError, std::string("加载 ONNX 模型失败: ") + e.what());
    }
}

}  // namespace

void detail::register_onnxruntime_backend() { register_backend("onnxruntime", make_onnxruntime); }

}  // namespace my::infer

#endif  // MYLIB_HAVE_ONNXRUNTIME
