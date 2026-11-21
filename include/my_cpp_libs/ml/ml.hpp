#pragma once
// =============================================================================
//  my::ml —— sklearn（及其它训练框架）导出的表格模型推理
//
//  背景：模型训练在 Python 侧，板端只需要"给一行特征、返回类别"。
//  导出链路是 skl2onnx（见 tools/sklearn_export.py）：
//
//      sklearn 模型 --skl2onnx--> model.onnx  --my::infer(ORT)--> 本组件
//                                  model.json   （特征名/类别名/版本）
//
//  为什么要有这一层，而不是直接用 my::infer：
//    1. sklearn 分类器是**双输出**（label int64 + probabilities f32），
//       infer 的 run_single_f32() 只支持 1 进 1 出；
//    2. 表格模型的特征**顺序**错了不会报错，只会输出垃圾；sidecar 记录
//       n_features/features，predict() 会校验**数量**，特征名用于调用方按
//       模型顺序对齐（名字本身不做逐项匹配，因为 predict 收的是 vector）；
//    3. 类别 id → 类别名（"setosa"）的映射要跟着模型走，不能写死在业务里。
//
//  不引入任何新的 C++ 三方库：ONNX 走已有的 ORT 后端，元数据走已有的 nlohmann/json。
//
//  线程安全：与 infer::Session 相同 —— **一个 Model 不能多线程并发 predict**，
//  多线程请每个线程各自 load 一个。
// =============================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "my_cpp_libs/core/status.hpp"
#include "my_cpp_libs/export.hpp"
#include "my_cpp_libs/infer/infer.hpp"

namespace my::ml {

enum class Kind { Classifier, Regressor };

/// 模型元数据：来自 sidecar（models/ml/*.json），没有 sidecar 时只填能从图里读到的部分
struct Meta {
    Kind                     kind = Kind::Classifier;
    std::string              format;        ///< sidecar 的 format 字段，如 "my_cpp_libs.ml/1"
    std::string              sidecar_path;  ///< 空 = 没有 sidecar
    int                      n_features = 0;
    std::vector<std::string> features;      ///< 特征名（按模型要求的顺序）
    std::vector<std::string> classes;       ///< 类别显示名（分类器）
    std::vector<std::int64_t> class_ids;    ///< 类别 id；skl2onnx 一般是 0..C-1
    std::string              input_name;
    std::string              label_output;  ///< 分类器的 int 输出（回归器为空）
    std::string              score_output;  ///< 概率 / 回归值的 float 输出
    /// sidecar 里记录的 .onnx 的 sha256（小写十六进制；没记录则为空）。
    /// load() 默认会拿它核对模型文件，见 load() 的 verify_hash 参数。
    std::string              onnx_sha256;
};

struct Prediction {
    std::int64_t             label = 0;   ///< 分类：类别 id；回归：无意义
    std::string              label_name;  ///< 有 classes 时填
    std::vector<float>       scores;      ///< 分类：概率（长度 = 类别数）；回归：输出

    /// 回归模型取第一个输出
    float value() const noexcept { return scores.empty() ? 0.0f : scores.front(); }
    /// 分类模型取概率最大的下标
    std::size_t argmax() const noexcept;
};

class Model {
public:
    Model() = default;
    Model(Model&&) noexcept            = default;
    Model& operator=(Model&&) noexcept = default;
    Model(const Model&)                = delete;
    Model& operator=(const Model&)     = delete;

    /// 加载模型。
    /// @param onnx_path   .onnx 路径
    /// @param sidecar     .json 路径；留空则自动找同名 .json。
    ///        显式给出但文件不存在时返回 Errc::NotFound；自动推导的 .json 缺失
    ///        视为"没有 sidecar"（模型维度必须静态）。
    ///        sidecar 解析失败（格式/字段类型错误）返回 Errc::ParseError，不会抛异常。
    ///
    /// 没有 sidecar 也能用（类别名/特征名缺失），但特征数必须是静态的。
    ///
    /// @param verify_hash sidecar 里记了 `sha256` 时，是否核对模型文件。
    ///        默认**核对**：模型被截断/被换掉/量化后忘了更新 sidecar 都会在这里报出来，
    ///        而不是等到推理结果莫名其妙。想跳过（比如故意换模型做实验）就传 false。
    MYLIB_API static Result<Model> load(const std::string& onnx_path,
                                        const std::string& sidecar_path = std::string(),
                                        bool               verify_hash  = true);

    /// 批量推理：features 长度必须是 n_features 的整数倍，样本数 = size / n_features。
    /// 只校验数量；调用方必须按 `meta().features` 的顺序排特征（名字不做逐项比对）。
    MYLIB_API Result<std::vector<Prediction>> predict(const std::vector<float>& features);

    /// 单样本推理（features.size() 必须等于 n_features）
    MYLIB_API Result<Prediction> predict_one(const std::vector<float>& features);

    const Meta& meta() const noexcept { return meta_; }
    MYLIB_API const char* backend() const noexcept;
    bool        valid() const noexcept { return session_ != nullptr; }

private:
    Status      load_sidecar(const std::string& path);
    std::size_t class_index(std::int64_t label) const noexcept;

    std::unique_ptr<infer::Session> session_;
    Meta                            meta_;
};

/// 把模型直接推到板端时用的"自检"：跑一遍 golden 文件（可选，测试用）
/// 这里不提供，业务侧用 predict() 即可。

}  // namespace my::ml
