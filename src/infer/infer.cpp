#include "my_cpp_libs/infer/infer.hpp"

#include "backends.hpp"

#include "my_cpp_libs/core/half.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace my::infer {

// =============================================================================
//  DType / Shape / Tensor
// =============================================================================
const char* dtype_name(DType type) noexcept {
    switch (type) {
        case DType::Float32: return "f32";
        case DType::Float16: return "f16";
        case DType::Float64: return "f64";
        case DType::Int8: return "i8";
        case DType::UInt8: return "u8";
        case DType::Int32: return "i32";
        case DType::Int64: return "i64";
        case DType::Bool: return "bool";
        case DType::Unknown: default: return "unknown";
    }
}

std::size_t dtype_size(DType type) noexcept {
    switch (type) {
        case DType::Float32: return 4;
        case DType::Float16: return 2;
        case DType::Float64: return 8;
        case DType::Int8: return 1;
        case DType::UInt8: return 1;
        case DType::Int32: return 4;
        case DType::Int64: return 8;
        case DType::Bool: return 1;
        case DType::Unknown: default: return 0;
    }
}

std::int64_t Shape::elements() const noexcept {
    if (dims.empty()) return 0;
    std::int64_t n = 1;
    for (std::int64_t d : dims) {
        if (d <= 0) return 0;  // 动态维
        n *= d;
    }
    return n;
}

std::string Shape::to_string() const {
    std::string s = "[";
    for (std::size_t i = 0; i < dims.size(); ++i) {
        if (i) s += ", ";
        s += std::to_string(dims[i]);
    }
    s += "]";
    return s;
}

std::size_t Tensor::byte_size() const noexcept {
    if (bytes != 0) return bytes;
    const std::int64_t n = shape.elements();
    if (n <= 0) return 0;
    return static_cast<std::size_t>(n) * dtype_size(type);
}

// =============================================================================
//  Session 的通用便捷路径
// =============================================================================
Status Session::run_single(const Tensor& input, Tensor& output) {
    const auto& ins  = inputs();
    const auto& outs = outputs();
    if (ins.size() != 1 || outs.size() != 1) {
        return Status(Errc::Unsupported,
                      "run_single 要求模型恰好 1 个输入、1 个输出，实际 " +
                          std::to_string(ins.size()) + "/" + std::to_string(outs.size()));
    }

    std::vector<Feed> feeds;
    feeds.push_back(Feed{ins[0].name, input});

    std::vector<Outcome> outcomes;
    Status st = run(feeds, outcomes);
    if (!st) return st;
    if (outcomes.empty()) {
        return Status(Errc::RuntimeError, "推理没有返回任何输出");
    }
    output = outcomes[0].tensor;
    return Ok();
}

Result<std::vector<float>> Session::run_single_f32(const std::vector<float>& input,
                                                   const Shape&                input_shape) {
    Tensor in;
    in.type  = DType::Float32;
    in.shape = input_shape;
    in.data  = const_cast<float*>(input.data());
    in.bytes = input.size() * sizeof(float);

    Tensor out;
    Status st = run_single(in, out);
    if (!st) return st;

    if (out.type != DType::Float32) {
        return Status(Errc::Unsupported,
                      std::string("输出类型不是 f32，而是 ") + dtype_name(out.type));
    }
    const std::size_t n = out.byte_size() / sizeof(float);
    const float*      p = static_cast<const float*>(out.data);
    return std::vector<float>(p, p + n);
}

// =============================================================================
//  模型探查 / 张量转换
// =============================================================================
Result<ModelInfo> inspect_model(const Options& options) {
    auto session = create_session(options);
    if (!session) return session.status();

    ModelInfo info;
    info.backend    = session.value()->backend_name();
    info.model_path = options.model_path;
    info.inputs     = session.value()->inputs();
    info.outputs    = session.value()->outputs();
    return info;
}

Result<std::vector<float>> tensor_to_float32(const Tensor& tensor) {
    if (tensor.data == nullptr) {
        return Status(Errc::InvalidArgument, "张量数据为空");
    }
    const std::size_t bytes = tensor.byte_size();
    if (bytes == 0) {
        return Status(Errc::InvalidArgument, "张量字节数为 0（形状可能含动态维）");
    }

    const std::size_t elem = dtype_size(tensor.type);
    if (elem == 0) {
        return Status(Errc::Unsupported, "未知的张量类型");
    }
    const std::size_t count = bytes / elem;

    std::vector<float> out;
    out.resize(count);

    switch (tensor.type) {
        case DType::Float32: {
            std::memcpy(out.data(), tensor.data, count * sizeof(float));
            return out;
        }
        case DType::Float16: {
            const auto* p = static_cast<const std::uint16_t*>(tensor.data);
            for (std::size_t i = 0; i < count; ++i) out[i] = my::half_to_float(p[i]);
            return out;
        }
        case DType::Float64: {
            const auto* p = static_cast<const double*>(tensor.data);
            for (std::size_t i = 0; i < count; ++i) out[i] = static_cast<float>(p[i]);
            return out;
        }
        case DType::Int8: {
            const auto* p = static_cast<const std::int8_t*>(tensor.data);
            for (std::size_t i = 0; i < count; ++i) out[i] = static_cast<float>(p[i]);
            return out;
        }
        case DType::UInt8: {
            const auto* p = static_cast<const std::uint8_t*>(tensor.data);
            for (std::size_t i = 0; i < count; ++i) out[i] = static_cast<float>(p[i]);
            return out;
        }
        case DType::Int32: {
            const auto* p = static_cast<const std::int32_t*>(tensor.data);
            for (std::size_t i = 0; i < count; ++i) out[i] = static_cast<float>(p[i]);
            return out;
        }
        case DType::Int64: {
            const auto* p = static_cast<const std::int64_t*>(tensor.data);
            for (std::size_t i = 0; i < count; ++i) out[i] = static_cast<float>(p[i]);
            return out;
        }
        case DType::Bool: {
            const auto* p = static_cast<const std::uint8_t*>(tensor.data);
            for (std::size_t i = 0; i < count; ++i) out[i] = p[i] ? 1.0f : 0.0f;
            return out;
        }
        case DType::Unknown:
        default:
            return Status(Errc::Unsupported, "不支持的张量类型");
    }
}

std::string tensor_to_string(const Tensor& tensor) {
    std::string s = dtype_name(tensor.type);
    s += " ";
    s += tensor.shape.to_string();
    s += " ";
    s += std::to_string(tensor.byte_size());
    s += " bytes";
    return s;
}

// =============================================================================
//  后端注册表
// =============================================================================
namespace {

struct Registry {
    std::mutex                                     mutex;
    std::unordered_map<std::string, SessionFactory> factories;
    std::vector<std::string>                       order;  // 注册顺序，用于默认挑选
};

Registry& registry() {
    static Registry r;
    return r;
}

/// 内置后端只注册一次。放在函数里而不是全局静态对象，
/// 是为了规避"静态库中仅含静态注册对象的 .obj 被链接器丢弃"的问题。
void ensure_builtin_backends() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        detail::register_stub_backend();
#if defined(MYLIB_HAVE_ONNXRUNTIME)
        detail::register_onnxruntime_backend();
#endif
#if defined(MYLIB_HAVE_NCNN)
        detail::register_ncnn_backend();
#endif
    });
}

}  // namespace

void register_backend(std::string name, SessionFactory factory) {
    if (name.empty() || !factory) return;
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    if (r.factories.find(name) == r.factories.end()) {
        r.order.push_back(name);
    }
    r.factories[name] = std::move(factory);
}

std::vector<std::string> available_backends() {
    ensure_builtin_backends();
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    return r.order;
}

bool has_backend(std::string_view name) noexcept {
    if (name.empty()) return false;
    ensure_builtin_backends();
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    return r.factories.find(std::string(name)) != r.factories.end();
}

const char* build_backend() noexcept {
#if defined(MYLIB_HAVE_ONNXRUNTIME)
    return "onnxruntime";
#elif defined(MYLIB_HAVE_NCNN)
    return "ncnn";
#else
    return "stub";
#endif
}

Result<std::unique_ptr<Session>> create_session(const Options& options) {
    Options       opt = options;
    std::string   chosen = opt.backend;

    if (chosen.empty()) {
        if (opt.model_path.empty()) {
            // 没有模型文件就不必加载真实后端，stub 足够跑通调用链
            chosen = "stub";
        } else {
            // 有模型就只考虑"真能加载模型"的后端。
            // 绝不能悄悄退回 stub —— 那会让"模型跑通了"变成假象。
            // 按扩展名给个默认倾向：.param 是 ncnn，其余（.onnx）优先 onnxruntime。
            const bool looks_like_ncnn = opt.model_path.size() > 6 &&
                                         opt.model_path.compare(opt.model_path.size() - 6, 6,
                                                                ".param") == 0;

            const char* order_normal[] = {"onnxruntime", "ncnn"};
            const char* order_ncnn[]   = {"ncnn", "onnxruntime"};
            const char** order = looks_like_ncnn ? order_ncnn : order_normal;

            for (int i = 0; i < 2; ++i) {
                if (has_backend(order[i])) {
                    chosen = order[i];
                    break;
                }
            }
            if (chosen.empty()) {
                return Status(Errc::Unsupported,
                              "当前构建没有能加载模型的推理后端（编译期后端: " +
                                  std::string(build_backend()) +
                                  "）。x86 请用 MSVC 构建以启用 onnxruntime；"
                                  "armv7 需要自行交叉编译 ONNX Runtime 或接入 ncnn。");
            }
        }
    }

    if (chosen.empty() || !has_backend(chosen)) {
        std::string available;
        for (const auto& n : available_backends()) {
            if (!available.empty()) available += ", ";
            available += n;
        }
        return Status(Errc::Unsupported,
                      "推理后端 '" + chosen + "' 不可用（已注册: " +
                          (available.empty() ? "无" : available) + "，编译期后端: " + build_backend() +
                          "）");
    }

    SessionFactory factory;
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        factory = r.factories.at(chosen);
    }

    opt.backend = chosen;
    return factory(opt);
}

}  // namespace my::infer
