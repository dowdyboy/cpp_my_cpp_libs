#include "my_cpp_libs/core/hash.hpp"
#include "my_cpp_libs/core/path_utf8.hpp"
#include "my_cpp_libs/ml/ml.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <string>

#if defined(MYLIB_ML_HAS_JSON)
#  include "my_cpp_libs/json/json.hpp"
#endif

namespace my::ml {
namespace {

/// 把 onnx 路径换成同名 .json。
///
/// 只在**文件名**里找扩展名：`models/v1.0/net` 的目录名带点，按整条路径找
/// 最后一个 '.' 会拼出 `models/v1.json`，sidecar 静默丢失。
std::string sibling_json(const std::string& onnx_path) {
    const auto slash = onnx_path.find_last_of("/\\");
    const auto dot   = onnx_path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return onnx_path + ".json";
    }
    return onnx_path.substr(0, dot) + ".json";
}

bool file_exists(const std::string& path) {
    std::ifstream f(my::core::to_path(path), std::ios::binary);
    return static_cast<bool>(f);
}

}  // namespace

std::size_t Prediction::argmax() const noexcept {
    if (scores.empty()) return 0;
    return static_cast<std::size_t>(
        std::max_element(scores.begin(), scores.end()) - scores.begin());
}

// -----------------------------------------------------------------------------
//  加载
// -----------------------------------------------------------------------------
Result<Model> Model::load(const std::string& onnx_path, const std::string& sidecar_path,
                          bool verify_hash) {
    if (onnx_path.empty()) return Status(Errc::InvalidArgument, "模型路径为空");
    if (!file_exists(onnx_path)) {
        return Status(Errc::NotFound, "模型文件不存在: " + onnx_path);
    }

    infer::Options opt;
    opt.model_path = onnx_path;
    auto session = infer::create_session(opt);
    if (!session) return session.status();

    Model m;
    m.session_ = std::move(session.value());

    const auto& ins = m.session_->inputs();
    const auto& outs = m.session_->outputs();
    if (ins.size() != 1) {
        return Status(Errc::Unsupported,
                      "ml 只支持单输入模型，当前有 " + std::to_string(ins.size()) + " 个输入");
    }
    if (ins[0].type != infer::DType::Float32) {
        return Status(Errc::Unsupported,
                      std::string("输入 dtype 必须是 float32，当前是 ") +
                          infer::dtype_name(ins[0].type));
    }
    m.meta_.input_name = ins[0].name;

    // 特征数：静态维度直接读；动态维（-1）留给 sidecar
    if (ins[0].shape.rank() >= 2) {
        const auto d = ins[0].shape.dims[1];
        if (d > 0) m.meta_.n_features = static_cast<int>(d);
    }

    // 输出布局：sklearn 分类器 = label(int64/32, rank1) + probabilities(float32, rank2)
    //           回归器     = 单个 float32 输出
    for (const auto& p : outs) {
        if (m.meta_.label_output.empty() &&
            (p.type == infer::DType::Int64 || p.type == infer::DType::Int32) &&
            p.shape.rank() == 1) {
            m.meta_.label_output = p.name;
        } else if (m.meta_.score_output.empty() && p.type == infer::DType::Float32) {
            m.meta_.score_output = p.name;
        }
    }
    if (m.meta_.score_output.empty()) {
        return Status(Errc::Unsupported, "模型没有 float32 输出，不是支持的表格模型");
    }
    m.meta_.kind = m.meta_.label_output.empty() ? Kind::Regressor : Kind::Classifier;

    // sidecar
    std::string sc = sidecar_path.empty() ? sibling_json(onnx_path) : sidecar_path;
    if (file_exists(sc)) {
        const Status st = m.load_sidecar(sc);
        if (!st) return st;
    }

    // 模型自校验：sidecar 里记了 sha256 就核对一遍。
    // 这条的价值在于"换了模型文件但忘了更新 sidecar"这种事故 —— 否则表现是
    // 推理结果莫名其妙，而不是一个明确的加载错误。
    if (verify_hash && !m.meta_.onnx_sha256.empty()) {
        auto actual = my::hash::sha256_file_hex(onnx_path);
        if (!actual) return actual.status();
        if (!my::hash::digest_equal(actual.value(), m.meta_.onnx_sha256)) {
            return Status(Errc::ParseError,
                          "模型文件与 sidecar 记录的 sha256 不一致: " + onnx_path + "（实际 " +
                              actual.value() + "，期望 " + m.meta_.onnx_sha256 +
                              "）。如果是有意替换模型，请同步更新 sidecar 或传 verify_hash=false");
        }
    }

    if (m.meta_.n_features <= 0) {
        return Status(Errc::InvalidArgument,
                      "无法确定特征数（输入是动态维度）：请提供同名的 .json sidecar");
    }
    return Result<Model>(std::move(m));
}

Status Model::load_sidecar(const std::string& path) {
#if !defined(MYLIB_ML_HAS_JSON)
    // 没有 json 组件：忽略 sidecar，模型本身照样能跑，只是拿不到特征名/类别名。
    // （特征数此时必须能从模型输入维度读出来，否则 load() 会报错。）
    (void)path;
    return Ok();
#else
    auto doc = my::json::Document::parse_file(path);
    if (!doc) return doc.status();

    const auto& d = doc.value();

    const std::string fmt = d.get_or<std::string>("format", "");
    if (fmt.rfind("my_cpp_libs.ml/", 0) != 0) {
        return Status(Errc::ParseError, "sidecar 格式不匹配（缺少 format=my_cpp_libs.ml/*）: " + path);
    }
    meta_.format       = fmt;
    meta_.sidecar_path = path;

    const std::string kind = d.get_or<std::string>("kind", "");
    if (kind == "classifier") {
        meta_.kind = Kind::Classifier;
    } else if (kind == "regressor") {
        meta_.kind = Kind::Regressor;
    } else {
        return Status(Errc::ParseError, "sidecar 的 kind 只能是 classifier/regressor: " + kind);
    }

    // 模型文件摘要（导出脚本写进去的）。没有这个字段就不校验 —— 兼容老 sidecar。
    // 注意：导出脚本把摘要写在 `model.sha256`（与 `model.onnx` 同级），不是顶层。
    // 0.1.7~0.1.9 这里读的是顶层 "sha256"，导致校验永远不执行；顶层读取保留为
    // 兼容老 sidecar 的退路。
    meta_.onnx_sha256 = d.get_or<std::string>("model.sha256", "");
    if (meta_.onnx_sha256.empty()) {
        meta_.onnx_sha256 = d.get_or<std::string>("sha256", "");
    }
    if (!meta_.onnx_sha256.empty() && meta_.onnx_sha256.size() != 64) {
        return Status(Errc::ParseError,
                      "sidecar 的 model.sha256 必须是 64 位十六进制，实际长度 " +
                          std::to_string(meta_.onnx_sha256.size()) + ": " + path);
    }

    // ---- 输入 ----
    if (auto r = d.get<std::string>("input.name"); r) {
        // 图里的端口名以模型为准，这里只做一致性校验
        if (r.value() != meta_.input_name) {
            return Status(Errc::ParseError,
                          "sidecar 的 input.name(" + r.value() + ") 与模型端口(" +
                              meta_.input_name + ") 不一致");
        }
    }
    const int nf = d.get_or<int>("input.n_features", 0);
    if (nf > 0) {
        if (meta_.n_features > 0 && meta_.n_features != nf) {
            return Status(Errc::ParseError,
                          "sidecar 的 n_features(" + std::to_string(nf) +
                              ") 与模型输入维度(" + std::to_string(meta_.n_features) + ") 不一致");
        }
        meta_.n_features = nf;
    }
    if (auto r = d.get<std::vector<std::string>>("input.features"); r) {
        meta_.features = r.value();
        if (!meta_.features.empty() &&
            meta_.features.size() != static_cast<std::size_t>(meta_.n_features)) {
            return Status(Errc::ParseError,
                          "sidecar 的 features 数量与 n_features 不一致");
        }
    }

    // ---- 输出 ----
    // skl2onnx 的输出名不一定叫 label/probabilities，以 sidecar 里记录的为准，
    // 但要回模型里确认这些名字真的存在。
    if (auto items = d.get<nlohmann::json>("output.items"); items) {
        for (const auto& it : items.value()) {
            if (!it.contains("name") || !it.contains("dtype")) continue;
            const std::string name = it["name"].get<std::string>();
            const std::string dt   = it["dtype"].get<std::string>();
            bool found = false;
            for (const auto& p : session_->outputs()) {
                if (p.name == name) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return Status(Errc::ParseError,
                              "sidecar 里的输出 " + name + " 在模型里不存在（模型换过？）");
            }
            if (dt == "int64" || dt == "int32") {
                meta_.label_output = name;
            } else if (dt == "float") {
                meta_.score_output = name;
            }
        }
    }
    if (meta_.score_output.empty()) {
        return Status(Errc::ParseError, "sidecar 里没有 float 输出");
    }
    meta_.kind = meta_.label_output.empty() ? Kind::Regressor : Kind::Classifier;

    if (auto r = d.get<std::vector<std::string>>("output.classes"); r) {
        meta_.classes = r.value();
    }
    if (auto r = d.get<std::vector<std::int64_t>>("output.class_ids"); r) {
        meta_.class_ids = r.value();
    }
    if (meta_.kind == Kind::Classifier && !meta_.classes.empty() &&
        meta_.classes.size() != meta_.class_ids.size() && !meta_.class_ids.empty()) {
        return Status(Errc::ParseError, "sidecar 的 classes 与 class_ids 数量不一致");
    }
    return Ok();
#endif
}

std::size_t Model::class_index(std::int64_t label) const noexcept {
    if (meta_.classes.empty()) return 0;
    // 优先按 class_ids 找（类别 id 不连续时，输出的是 id 而不是下标）
    for (std::size_t i = 0; i < meta_.class_ids.size(); ++i) {
        if (meta_.class_ids[i] == label) return i;
    }
    if (label >= 0 && static_cast<std::size_t>(label) < meta_.classes.size()) {
        return static_cast<std::size_t>(label);
    }
    return meta_.classes.size();  // 越界：调用方据此判断"没有名字"
}

const char* Model::backend() const noexcept {
    return session_ ? session_->backend_name() : "none";
}

// -----------------------------------------------------------------------------
//  推理
// -----------------------------------------------------------------------------
Result<std::vector<Prediction>> Model::predict(const std::vector<float>& features) {
    if (!session_) return Status(Errc::NotInitialized, "Model 未加载");
    const std::size_t nf = static_cast<std::size_t>(meta_.n_features);
    if (nf == 0) return Status(Errc::NotInitialized, "特征数未知");
    if (features.empty() || features.size() % nf != 0) {
        return Status(Errc::InvalidArgument,
                      "特征数组长度 " + std::to_string(features.size()) +
                          " 不是 n_features(" + std::to_string(nf) + ") 的整数倍");
    }
    const std::size_t n = features.size() / nf;

    infer::Tensor tensor(infer::DType::Float32,
                         infer::Shape{static_cast<std::int64_t>(n),
                                      static_cast<std::int64_t>(nf)},
                         const_cast<float*>(features.data()),
                         features.size() * sizeof(float));

    std::vector<infer::Feed>    feeds{{meta_.input_name, tensor}};
    std::vector<infer::Outcome> outcomes;
    auto st = session_->run(feeds, outcomes);
    if (!st) return st;

    const infer::Outcome* score = nullptr;
    const infer::Outcome* label = nullptr;
    for (const auto& o : outcomes) {
        if (o.name == meta_.score_output) score = &o;
        if (!meta_.label_output.empty() && o.name == meta_.label_output) label = &o;
    }
    if (score == nullptr && outcomes.size() == 1) score = &outcomes.front();
    if (score == nullptr) {
        return Status(Errc::RuntimeError, "推理结果里找不到输出端口: " + meta_.score_output);
    }
    if (score->tensor.type != infer::DType::Float32) {
        return Status(Errc::Unsupported,
                      std::string("输出 dtype 不是 float32: ") +
                          infer::dtype_name(score->tensor.type));
    }

    const std::size_t total = score->tensor.byte_size() / sizeof(float);
    if (n == 0 || total % n != 0) {
        return Status(Errc::RuntimeError, "输出元素数与样本数不匹配");
    }
    const std::size_t per_sample = total / n;

    std::vector<float> flat(total);
    std::memcpy(flat.data(), score->tensor.data, total * sizeof(float));

    std::vector<std::int64_t> labels(n, 0);
    if (label != nullptr) {
        const auto& t = label->tensor;
        // 按真实 dtype 算每个元素占几字节：Int64/Int32/Float32 都要校验，
        // 否则输出被裁短时会越界读（之前只查了 Int64）。
        std::size_t elem = 0;
        switch (t.type) {
            case infer::DType::Int64:   elem = sizeof(std::int64_t); break;
            case infer::DType::Int32:   elem = sizeof(std::int32_t); break;
            case infer::DType::Float32: elem = sizeof(float); break;
            default: break;
        }
        if (elem == 0) {
            return Status(Errc::Unsupported,
                          std::string("label 输出 dtype 不支持: ") + infer::dtype_name(t.type));
        }
        if (t.byte_size() < n * elem) {
            return Status(Errc::RuntimeError,
                          "label 输出长度不足（需要 " + std::to_string(n * elem) + " 字节，实际 " +
                              std::to_string(t.byte_size()) + "）");
        }
        for (std::size_t i = 0; i < n; ++i) {
            switch (t.type) {
                case infer::DType::Int64:
                    labels[i] = static_cast<const std::int64_t*>(t.data)[i];
                    break;
                case infer::DType::Int32:
                    labels[i] = static_cast<const std::int32_t*>(t.data)[i];
                    break;
                case infer::DType::Float32:
                    labels[i] = static_cast<std::int64_t>(
                        std::lround(static_cast<const float*>(t.data)[i]));
                    break;
                default:
                    return Status(Errc::Unsupported,
                                  std::string("label 输出 dtype 不支持: ") +
                                      infer::dtype_name(t.type));
            }
        }
    }

    std::vector<Prediction> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i].label  = labels[i];
        out[i].scores.assign(flat.begin() + static_cast<std::ptrdiff_t>(i * per_sample),
                             flat.begin() + static_cast<std::ptrdiff_t>((i + 1) * per_sample));
        if (!meta_.classes.empty()) {
            const std::size_t idx = class_index(labels[i]);
            if (idx < meta_.classes.size()) out[i].label_name = meta_.classes[idx];
        }
    }

    // 分类器：输出列数应当等于类别数（动态 batch 时也成立）
    if (meta_.kind == Kind::Classifier && !meta_.classes.empty() &&
        per_sample != meta_.classes.size()) {
        return Status(Errc::RuntimeError,
                      "输出列数 " + std::to_string(per_sample) + " 与类别数 " +
                          std::to_string(meta_.classes.size()) + " 不一致");
    }
    return out;
}

Result<Prediction> Model::predict_one(const std::vector<float>& features) {
    auto r = predict(features);
    if (!r) return r.status();
    if (r.value().size() != 1) {
        return Status(Errc::InvalidArgument,
                      "predict_one 只接受一行特征，实际得到 " +
                          std::to_string(r.value().size()) + " 行");
    }
    return r.value().front();
}

}  // namespace my::ml
