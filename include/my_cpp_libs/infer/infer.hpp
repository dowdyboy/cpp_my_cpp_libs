#pragma once
// =============================================================================
//  my::infer —— 平台无关的推理接口
//
//  为什么需要这一层：ONNX Runtime 官方只提供 Windows-x64 / Linux-x64 /
//  Linux-aarch64 等预编译包，Zynq-7000（armv7）没有官方二进制；
//  而 ncnn 这类轻量框架又只吃自己的模型格式。
//  把推理抽象成 Session 接口 + 可注册后端后：
//    * 主路线：ONNX Runtime（算子覆盖最全，x86 开箱可用、armv7 可自编）
//    * 备选：ncnn（体积小，但需要 onnx2ncnn 转换，算子覆盖有限）
//    * 兜底：stub（回显输入，用于没有模型/没有库时跑通链路）
//  业务代码不需要知道用的是哪个后端。
//
//  后端注册：my::infer::register_backend("xxx", factory)
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "my_cpp_libs/core/status.hpp"
#include "my_cpp_libs/export.hpp"

namespace my::infer {

// =============================================================================
//  数据类型
// =============================================================================
enum class DType { Unknown = 0, Float32, Float16, Float64, Int8, UInt8, Int32, Int64, Bool };

MYLIB_API const char* dtype_name(DType type) noexcept;
MYLIB_API std::size_t dtype_size(DType type) noexcept;

// =============================================================================
//  形状
// =============================================================================
struct Shape {
    std::vector<std::int64_t> dims;

    Shape() = default;
    Shape(std::initializer_list<std::int64_t> d) : dims(d) {}
    explicit Shape(std::vector<std::int64_t> d) : dims(std::move(d)) {}

    std::size_t rank() const noexcept { return dims.size(); }

    /// 元素个数；含动态维（<0）时返回 0
    std::int64_t elements() const noexcept;

    bool operator==(const Shape& o) const noexcept { return dims == o.dims; }
    std::string to_string() const;
};

// =============================================================================
//  张量视图（不拥有内存）
// =============================================================================
struct Tensor {
    DType       type = DType::Float32;
    Shape       shape;
    void*       data = nullptr;   ///< 行优先连续内存
    std::size_t bytes = 0;        ///< 0 表示按 type*shape 推算

    Tensor() = default;
    Tensor(DType t, Shape s, void* p, std::size_t n = 0)
        : type(t), shape(std::move(s)), data(p), bytes(n) {}

    /// 实际字节数（bytes 为 0 时按 dtype*elements 推算）
    std::size_t byte_size() const noexcept;
    bool        valid() const noexcept { return data != nullptr && byte_size() > 0; }
};

/// 模型端口描述（输入/输出）
struct Port {
    std::string name;
    DType       type = DType::Float32;
    Shape       shape;
};

// =============================================================================
//  会话
// =============================================================================
struct Feed {
    std::string_view name;
    Tensor           tensor;
};

/// 输出张量的内存由 Session 拥有，在下一次 run() 之前有效
struct Outcome {
    std::string name;
    Tensor      tensor;
};

struct Options {
    /// 后端名；空串表示使用默认后端
    std::string backend;
    /// 模型路径（onnx / 后端自有格式）
    std::string model_path;
    int intra_threads = 1;
    int inter_threads = 1;
    /// 生成后端专有配置，键值透传
    std::vector<std::pair<std::string, std::string>> provider_options;
    /// 是否把推理细节打到 my::log（若日志组件可用）
    bool verbose = false;
};

class Session {
public:
    virtual ~Session() = default;

    virtual const char* backend_name() const noexcept = 0;

    virtual const std::vector<Port>& inputs() const noexcept  = 0;
    virtual const std::vector<Port>& outputs() const noexcept = 0;

    /// 执行一次推理。feeds 按端口名匹配；outcomes 由实现填充。
    virtual Status run(const std::vector<Feed>& feeds, std::vector<Outcome>& outcomes) = 0;

    /// 单输入单输出便捷路径：要求模型恰好 1 进 1 出
    Status run_single(const Tensor& input, Tensor& output);

    /// 单输入单输出 + 结果拷贝到 std::vector<float>
    Result<std::vector<float>> run_single_f32(const std::vector<float>& input,
                                              const Shape&                input_shape);
};

// =============================================================================
//  模型探查
// =============================================================================
struct ModelInfo {
    std::string       backend;
    std::string       model_path;
    std::vector<Port> inputs;
    std::vector<Port> outputs;
};

/// 打开模型并读取输入输出描述。
/// 这一步会真正加载一次模型，因此"能不能跑"在调用时就能给出确定答案，
/// 而不是等到第一次推理才炸。排查陌生模型时应当先调它。
MYLIB_API Result<ModelInfo> inspect_model(const Options& options);

// =============================================================================
//  张量数据转换
// =============================================================================
/// 把任意数值类型的张量转成 float32 数组（fp16 会做真转换，整型做数值转换）。
/// 后处理几乎都按 float 写，这个函数负责抹平类型差异。
MYLIB_API Result<std::vector<float>> tensor_to_float32(const Tensor& tensor);

/// 张量的可读描述，如 "f32 [1,3,224,224] 602112 bytes"
MYLIB_API std::string tensor_to_string(const Tensor& tensor);

// =============================================================================
//  后端注册表
// =============================================================================
using SessionFactory = std::function<Result<std::unique_ptr<Session>>(const Options&)>;

/// 注册一个后端（同名后注册者覆盖先注册者）
MYLIB_API void register_backend(std::string name, SessionFactory factory);

/// 已注册的后端名
MYLIB_API std::vector<std::string> available_backends();

MYLIB_API bool has_backend(std::string_view name) noexcept;

/// 创建会话。Options::backend 为空时按优先级挑选：
///   onnxruntime -> ncnn -> stub
MYLIB_API Result<std::unique_ptr<Session>> create_session(const Options& options);

/// 编译期已知的推理能力描述，例如 "onnxruntime" / "stub"
MYLIB_API const char* build_backend() noexcept;

}  // namespace my::infer
