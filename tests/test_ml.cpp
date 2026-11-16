// =============================================================================
//  my::ml 单元测试
//
//  测试数据是 models/ml/ 下的真实 sklearn 模型（由 tools/train_ml_models.py
//  用 t2xpu 环境导出），配套 .golden.tsv 是**sklearn 自己**算出来的预测结果。
//  这样对拍链是：
//      sklearn 训练 --skl2onnx--> onnx  --my::ml(ORT)--> C++ 结果  ==  sklearn 结果
//
//  没有 ONNX Runtime 的构建（MinGW / 板端不带 ORT）会直接跳过，
//  因为 ml 组件本来就依赖 infer 的 ORT 后端。
// =============================================================================

#include "my_cpp_libs/ml/ml.hpp"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "test_framework.hpp"

MYTEST_SUITE("ml");

// CHECK_OK_VALUE 绑定出来的是 const&，而 Model::predict 不是 const 成员
// （Session::run 本身非 const），所以这里单独包一个。
#define ML_LOAD(name, ...)                      \
    auto name##_res = my::ml::Model::load(__VA_ARGS__); \
    CHECK_RESULT(name##_res);                    \
    auto& name = name##_res.value()

#ifndef MYLIB_TEST_MODELS_DIR
#  define MYLIB_TEST_MODELS_DIR "models"
#endif

namespace {

/// 模型目录：优先读环境变量，方便在板端/别的机器上跑同一份二进制
std::string models_root() {
    if (const char* env = std::getenv("MYLIB_TEST_MODELS_DIR")) {
        if (*env != '\0') return env;
    }
    return MYLIB_TEST_MODELS_DIR;
}

const std::string kModelDir = models_root() + "/ml/";

bool ml_ready() {
    static const bool ok = my::infer::has_backend("onnxruntime");
    if (!ok) {
        std::printf("          （跳过：本构建没有 onnxruntime 后端，ml 组件无法推理）\n");
    }
    return ok;
}

struct GoldenRow {
    std::vector<float> features;
    std::int64_t       label = 0;
    std::vector<float> scores;
};

/// 读 .golden.tsv：`feat... label score...`（分类）或 `feat... value`（回归），
/// `#` 开头是注释。为了用一个解析器吃两种格式：
///   * 特征后面只剩 1 个数 -> 回归，值放进 scores[0]；
///   * >= 2 个数        -> 分类，第一个是 label，其余是概率。
std::vector<GoldenRow> read_golden(const std::string& path, int n_features) {
    std::vector<GoldenRow> rows;
    std::ifstream          in(path);
    if (!in) return rows;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        GoldenRow          r;
        r.features.resize(static_cast<std::size_t>(n_features));
        bool ok = true;
        for (int i = 0; i < n_features; ++i) {
            if (!(ss >> r.features[static_cast<std::size_t>(i)])) {
                ok = false;
                break;
            }
        }
        if (!ok) continue;

        std::vector<double> rest;
        double              v = 0.0;
        while (ss >> v) rest.push_back(v);
        if (rest.empty()) continue;

        if (rest.size() == 1) {
            // 回归：唯一的值就是模型输出
            r.label = 0;
            r.scores.push_back(static_cast<float>(rest[0]));
        } else {
            r.label = static_cast<std::int64_t>(std::llround(rest[0]));
            for (std::size_t k = 1; k < rest.size(); ++k) {
                r.scores.push_back(static_cast<float>(rest[k]));
            }
        }
        rows.push_back(std::move(r));
    }
    return rows;
}

}  // namespace

MYTEST_CASE(ml_load_iris_meta) {
    if (!ml_ready()) return;

    ML_LOAD(m, kModelDir + "iris_logreg.onnx");

    const auto& meta = m.meta();
    CHECK_EQ(static_cast<int>(meta.kind), static_cast<int>(my::ml::Kind::Classifier));
    CHECK_EQ(meta.n_features, 4);
    CHECK_EQ(meta.classes.size(), std::size_t{3});
    CHECK_EQ(meta.class_ids.size(), std::size_t{3});
    CHECK_EQ(meta.features.size(), std::size_t{4});
    CHECK(meta.features[0] == "sepal_length");
    CHECK(meta.classes[0] == "setosa");
    CHECK(meta.classes[2] == "virginica");
    CHECK(meta.label_output == "label");
    CHECK(meta.score_output == "probabilities");
    CHECK(meta.format == "my_cpp_libs.ml/1");
    CHECK(std::string(m.backend()) == "onnxruntime");
}

MYTEST_CASE(ml_iris_matches_golden) {
    if (!ml_ready()) return;

    ML_LOAD(m, kModelDir + "iris_logreg.onnx");
    const auto rows = read_golden(kModelDir + "iris_logreg.golden.tsv", 4);
    CHECK(rows.size() >= std::size_t{6});

    for (const auto& g : rows) {
        auto r = m.predict_one(g.features);
        CHECK_RESULT(r);
        const auto& p = r.value();
        CHECK_EQ(p.label, g.label);
        CHECK_EQ(p.scores.size(), g.scores.size());
        for (std::size_t k = 0; k < g.scores.size(); ++k) {
            CHECK_NEAR(p.scores[k], g.scores[k], 2e-4);
        }
        CHECK(p.label_name == m.meta().classes[static_cast<std::size_t>(p.label)]);
    }
}

MYTEST_CASE(ml_iris_batch) {
    if (!ml_ready()) return;

    ML_LOAD(m, kModelDir + "iris_logreg.onnx");
    const auto rows = read_golden(kModelDir + "iris_logreg.golden.tsv", 4);

    std::vector<float> batch;
    for (const auto& g : rows) batch.insert(batch.end(), g.features.begin(), g.features.end());

    CHECK_OK_VALUE(preds, m.predict(batch));
    CHECK_EQ(preds.size(), rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        CHECK_EQ(preds[i].label, rows[i].label);
        CHECK_NEAR(preds[i].argmax(), static_cast<double>(rows[i].label), 1e-9);
    }
}

MYTEST_CASE(ml_gbdt_matches_golden) {
    if (!ml_ready()) return;

    ML_LOAD(m, kModelDir + "gbdt_20f.onnx");
    CHECK_EQ(m.meta().n_features, 20);
    CHECK_EQ(m.meta().classes.size(), std::size_t{2});

    const auto rows = read_golden(kModelDir + "gbdt_20f.golden.tsv", 20);
    CHECK(!rows.empty());

    for (const auto& g : rows) {
        auto r = m.predict_one(g.features);
        CHECK_RESULT(r);
        CHECK_EQ(r.value().label, g.label);
        for (std::size_t k = 0; k < g.scores.size() && k < r.value().scores.size(); ++k) {
            CHECK_NEAR(r.value().scores[k], g.scores[k], 2e-4);
        }
    }
}

MYTEST_CASE(ml_ridge_regressor) {
    if (!ml_ready()) return;

    ML_LOAD(m, kModelDir + "ridge_10f.onnx");
    CHECK_EQ(static_cast<int>(m.meta().kind), static_cast<int>(my::ml::Kind::Regressor));
    CHECK_EQ(m.meta().n_features, 10);
    CHECK(m.meta().label_output.empty());

    const auto rows = read_golden(kModelDir + "ridge_10f.golden.tsv", 10);
    CHECK(!rows.empty());

    for (const auto& g : rows) {
        auto r = m.predict_one(g.features);
        CHECK_RESULT(r);
        CHECK_EQ(r.value().scores.size(), std::size_t{1});
        // golden 行里 label 字段其实存的是回归真值（round 之后只用来定位，不参与比较）
        const double expected = g.scores.empty() ? 0.0 : g.scores.front();
        CHECK_NEAR(r.value().value(), expected, 1e-2);
    }
}

MYTEST_CASE(ml_rejects_wrong_feature_count) {
    if (!ml_ready()) return;

    ML_LOAD(m, kModelDir + "iris_logreg.onnx");

    auto bad = m.predict_one({1.0f, 2.0f, 3.0f});  // 3 != 4
    CHECK(!bad);
    CHECK_EQ(bad.status().code(), my::Errc::InvalidArgument);

    auto bad2 = m.predict({1.0f, 2.0f, 3.0f, 4.0f, 5.0f});  // 5 不是 4 的整数倍
    CHECK(!bad2);
    CHECK_EQ(bad2.status().code(), my::Errc::InvalidArgument);
}

MYTEST_CASE(ml_missing_model_reports_not_found) {
    auto r = my::ml::Model::load(kModelDir + "definitely_not_here.onnx");
    CHECK(!r);
    CHECK_EQ(r.status().code(), my::Errc::NotFound);
}

MYTEST_CASE(ml_works_without_sidecar) {
    if (!ml_ready()) return;

    // sidecar 指向不存在的文件：模型本身维度是静态的，应该还能用，
    // 只是拿不到特征名/类别名。
    ML_LOAD(m, (kModelDir + "iris_logreg.onnx"), "no_such_meta.json");
    CHECK_EQ(m.meta().n_features, 4);
    CHECK(m.meta().classes.empty());
    CHECK(m.meta().sidecar_path.empty());

    auto r = m.predict_one({5.1f, 3.5f, 1.4f, 0.2f});
    CHECK_RESULT(r);
    CHECK_EQ(r.value().label, std::int64_t{0});  // setosa
    CHECK(r.value().label_name.empty());        // 没有类别名
}

MYTEST_CASE(ml_sidecar_mismatch_is_rejected) {
    if (!ml_ready()) return;

    // gbdt 的 sidecar 说 20 维，拿它配 iris（4 维）应当报错而不是"跑出垃圾"
    auto r = my::ml::Model::load(kModelDir + "iris_logreg.onnx", kModelDir + "gbdt_20f.json");
    CHECK(!r);
    CHECK_EQ(r.status().code(), my::Errc::ParseError);
}
