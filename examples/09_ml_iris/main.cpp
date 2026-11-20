// =============================================================================
//  example_ml_iris —— sklearn 模型在 C++ 侧的用法
//
//  模型由 tools/train_ml_models.py 生成（sklearn -> skl2onnx -> .onnx + .json）。
//  这里演示三件事：
//    1. 加载模型并打印元数据（特征名、类别名、backend）；
//    2. 单样本预测（类别名一起拿到）；
//    3. 批量预测（特征数组按行拼起来）。
//
//  运行（在仓库根目录）：
//    build/win-msvc/bin/Release/example_ml models/ml/iris_logreg.onnx
// =============================================================================

#include <cstdio>
#include <string>
#include <vector>

#include "my_cpp_libs/log/format.hpp"
#include "my_cpp_libs/log/log.hpp"
#include "my_cpp_libs/ml/ml.hpp"

namespace {

void print_prediction(const my::ml::Prediction& p, const std::vector<float>& features) {
    std::string line;
    for (float f : features) line += my::log::format("{:.1f} ", static_cast<double>(f));
    std::string probs;
    for (float s : p.scores) probs += my::log::format("{:.4f} ", static_cast<double>(s));
    my::log::info("{:<24} -> {:<12} (id={})  [{}]", line, p.label_name, p.label, probs);
}

}  // namespace

int main(int argc, char** argv) {
    my::log::Config cfg;
    cfg.level      = my::log::Level::Info;
    cfg.to_console = true;
    (void)my::log::init(cfg);

    const std::string model_path =
        (argc > 1) ? argv[1] : "models/ml/iris_logreg.onnx";

    auto model = my::ml::Model::load(model_path);
    if (!model) {
        my::log::error("加载模型失败: {}", model.status().to_string());
        return 1;
    }
    const auto& meta = model.value().meta();
    my::log::info("模型: {}", model_path);
    my::log::info("后端: {}  格式: {}", model.value().backend(), meta.format);
    my::log::info("输入: {}  float32 [?,{}]", meta.input_name, meta.n_features);
    my::log::info("输出: {} + {}", meta.label_output, meta.score_output);

    std::string feats;
    for (const auto& f : meta.features) feats += f + " ";
    my::log::info("特征顺序: {}", feats);
    std::string classes;
    for (const auto& c : meta.classes) classes += c + " ";
    my::log::info("类别: {}", classes);

    // ---- 单样本 ----
    const std::vector<std::vector<float>> samples = {
        {5.1f, 3.5f, 1.4f, 0.2f},  // setosa
        {6.0f, 3.4f, 4.5f, 1.6f},  // versicolor
        {6.1f, 3.0f, 4.9f, 1.8f},  // virginica
    };
    my::log::info("");
    my::log::info("== 单样本 ==");
    for (const auto& s : samples) {
        auto r = model.value().predict_one(s);
        if (!r) {
            my::log::error("预测失败: {}", r.status().to_string());
            continue;
        }
        print_prediction(r.value(), s);
    }

    // ---- 批量 ----
    std::vector<float> batch;
    for (const auto& s : samples) batch.insert(batch.end(), s.begin(), s.end());

    auto preds = model.value().predict(batch);
    if (!preds) {
        my::log::error("批量预测失败: {}", preds.status().to_string());
        my::log::shutdown();
        return 1;
    }
    my::log::info("");
    my::log::info("== 批量（{} 行，一次 run）==", preds.value().size());
    for (std::size_t i = 0; i < preds.value().size(); ++i) {
        my::log::info("  #{} label={} ({})", i, preds.value()[i].label,
                      preds.value()[i].label_name);
    }

    my::log::shutdown();
    return 0;
}
