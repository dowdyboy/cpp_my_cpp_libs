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

#include "my_cpp_libs/io/io.hpp"
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
    return my::infer::has_backend("onnxruntime");
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
    if (!ml_ready()) MYTEST_SKIP("没有可用的推理后端");

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
    // 0.1.9 之前读错键路径（顶层 sha256 vs model.sha256），这个字段恒为空，
    // 哈希校验静默不执行。长度断言是最便宜的守门方式。
    CHECK_EQ(meta.onnx_sha256.size(), std::size_t{64});
    CHECK(std::string(m.backend()) == "onnxruntime");
}

MYTEST_CASE(ml_iris_matches_golden) {
    if (!ml_ready()) MYTEST_SKIP("没有可用的推理后端");

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
    if (!ml_ready()) MYTEST_SKIP("没有可用的推理后端");

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
    if (!ml_ready()) MYTEST_SKIP("没有可用的推理后端");

    ML_LOAD(m, kModelDir + "gbdt_20f.onnx");
    CHECK_EQ(m.meta().n_features, 20);
    CHECK_EQ(m.meta().classes.size(), std::size_t{2});

    const auto rows = read_golden(kModelDir + "gbdt_20f.golden.tsv", 20);
    CHECK(!rows.empty());

    for (const auto& g : rows) {
        auto r = m.predict_one(g.features);
        CHECK_RESULT(r);
        CHECK_EQ(r.value().label, g.label);
        // 长度必须一致：否则"少返回几个分数"会被 for 的条件悄悄放过
    CHECK_EQ(r.value().scores.size(), g.scores.size());
    for (std::size_t k = 0; k < g.scores.size() && k < r.value().scores.size(); ++k) {
            CHECK_NEAR(r.value().scores[k], g.scores[k], 2e-4);
        }
    }
}

MYTEST_CASE(ml_ridge_regressor) {
    if (!ml_ready()) MYTEST_SKIP("没有可用的推理后端");

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
    if (!ml_ready()) MYTEST_SKIP("没有可用的推理后端");

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

// B5 回归：显式给了 sidecar 路径但文件不存在，必须 NotFound，而不是静默忽略。
// 这条不依赖 ORT：参数校验在创建推理 session 之前。
MYTEST_CASE(ml_missing_explicit_sidecar_is_not_found) {
    auto r = my::ml::Model::load(kModelDir + "iris_logreg.onnx", "no_such_meta.json");
    CHECK(!r);
    CHECK_EQ(r.status().code(), my::Errc::NotFound);
}

// 自动推导（sidecar 留空）时，同名 .json 缺失是合法的：
// 模型维度静态就能用，只是拿不到特征名/类别名。为了避免读到模型旁的 .json，
// 这里把 onnx 复制到一个没有 sidecar 的临时目录再加载。
MYTEST_CASE(ml_works_without_sidecar) {
    if (!ml_ready()) MYTEST_SKIP("没有可用的推理后端");

    auto onnx_txt = my::io::read_file(kModelDir + "iris_logreg.onnx");
    CHECK(onnx_txt.ok());
    if (!onnx_txt.ok()) return;

#if defined(_WIN32)
    const std::string dir = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") +
                            "\\mylib_ml_no_sidecar";
#else
    const std::string dir = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
                            "/mylib_ml_no_sidecar";
#endif
    const std::string onnx_path = dir + "/iris_logreg.onnx";
    (void)my::io::remove_all(dir);
    CHECK(my::io::create_directories(dir).ok());
    CHECK(my::io::write_file(onnx_path, onnx_txt.value()).ok());

    auto loaded = my::ml::Model::load(onnx_path);   // sidecar 留空：自动推导，缺失允许
    CHECK_RESULT(loaded);
    auto& m = loaded.value();
    CHECK_EQ(m.meta().n_features, 4);
    CHECK(m.meta().classes.empty());
    CHECK(m.meta().sidecar_path.empty());

    auto r = m.predict_one({5.1f, 3.5f, 1.4f, 0.2f});
    CHECK_RESULT(r);
    CHECK_EQ(r.value().label, std::int64_t{0});   // setosa
    CHECK(r.value().label_name.empty());         // 没有类别名

    (void)my::io::remove_all(dir);
}

// A9 回归：sidecar 字段类型写错（{"name":123}）必须变成 ParseError。
// 以前 load_sidecar 绕过 Document::get 直接 get<std::string>()，type_error 会从
// Model::load 逸出，公共 API 的"不抛异常"承诺失效。
MYTEST_CASE(ml_rejects_malformed_sidecar) {
    if (!ml_ready()) MYTEST_SKIP("没有可用的推理后端");

    auto onnx_txt = my::io::read_file(kModelDir + "iris_logreg.onnx");
    CHECK(onnx_txt.ok());
    if (!onnx_txt.ok()) return;

#if defined(_WIN32)
    const std::string dir = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") +
                            "\\mylib_ml_bad_sidecar";
#else
    const std::string dir = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
                            "/mylib_ml_bad_sidecar";
#endif
    const std::string onnx_path = dir + "/iris_logreg.onnx";
    const std::string json_path = dir + "/bad_sidecar.json";
    const std::string bad_json =
        "{\n"
        "  \"format\": \"my_cpp_libs.ml/1\",\n"
        "  \"kind\": \"classifier\",\n"
        "  \"input\": {\"n_features\": 4},\n"
        "  \"output\": {\"items\": [{\"name\": 123, \"dtype\": \"float\"}]}\n"
        "}\n";
    (void)my::io::remove_all(dir);
    CHECK(my::io::create_directories(dir).ok());
    CHECK(my::io::write_file(onnx_path, onnx_txt.value()).ok());
    CHECK(my::io::write_file(json_path, bad_json).ok());

    // 关键：不抛异常，返回 ParseError
    auto bad = my::ml::Model::load(onnx_path, json_path);
    CHECK(!bad);
    CHECK_EQ(bad.status().code(), my::Errc::ParseError);

    (void)my::io::remove_all(dir);
}

MYTEST_CASE(ml_sidecar_mismatch_is_rejected) {
    if (!ml_ready()) MYTEST_SKIP("没有可用的推理后端");

    // gbdt 的 sidecar 说 20 维，拿它配 iris（4 维）应当报错而不是"跑出垃圾"
    auto r = my::ml::Model::load(kModelDir + "iris_logreg.onnx", kModelDir + "gbdt_20f.json");
    CHECK(!r);
    CHECK_EQ(r.status().code(), my::Errc::ParseError);
}

MYTEST_CASE(ml_unloaded_model_reports_error) {
    // 不依赖 ORT：默认构造的 Model 永远处于"未加载"状态，错误路径必须可返回。
    my::ml::Model m;
    CHECK(!m.valid());
    CHECK(std::string(m.backend()) == "none");
    CHECK_EQ(m.meta().n_features, 0);

    auto one = m.predict_one({1.0f, 2.0f, 3.0f, 4.0f});
    CHECK(!one);
    CHECK_EQ(one.status().code(), my::Errc::NotInitialized);

    auto batch = m.predict({1.0f, 2.0f, 3.0f, 4.0f});
    CHECK(!batch);
    CHECK_EQ(batch.status().code(), my::Errc::NotInitialized);
}

// sidecar 里记的 sha256 必须与模型文件一致。
//
// 这条守的是"换了模型但忘了更新 sidecar"这类事故：以前的表现是推理结果莫名其妙，
// 现在是一个明确的加载错误。verify_hash=false 用于故意换模型做实验的场景。
MYTEST_CASE(ml_rejects_model_hash_mismatch) {
    if (!ml_ready()) MYTEST_SKIP("没有可用的推理后端");

    const std::string src_onnx = models_root() + "/ml/iris_logreg.onnx";
    const std::string src_json = models_root() + "/ml/iris_logreg.json";
    auto              onnx_txt = my::io::read_file(src_onnx);
    auto              json_txt = my::io::read_file(src_json);
    CHECK(onnx_txt.ok());
    CHECK(json_txt.ok());
    if (!onnx_txt.ok() || !json_txt.ok()) return;

    // 造一个哈希不对的 sidecar：把 sha256 的最后一位改掉
    std::string tampered = json_txt.value();
    const auto  pos      = tampered.find("\"sha256\"");
    CHECK(pos != std::string::npos);
    if (pos == std::string::npos) return;
    const auto quote = tampered.find('"', tampered.find(':', pos) + 1);
    CHECK(quote != std::string::npos);
    if (quote == std::string::npos) return;
    const char flipped = (tampered[quote + 1] == '0') ? '1' : '0';
    tampered[quote + 1] = flipped;

#if defined(_WIN32)
    const std::string dir = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "\\mylib_ml_hash";
#else
    const std::string dir = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/mylib_ml_hash";
#endif
    const std::string tmp_onnx = dir + "/iris_logreg.onnx";
    const std::string tmp_json = dir + "/iris_logreg.json";
    (void)my::io::create_directories(dir);
    CHECK(my::io::write_file(tmp_onnx, onnx_txt.value()).ok());
    CHECK(my::io::write_file(tmp_json, tampered).ok());

    auto bad = my::ml::Model::load(tmp_onnx, tmp_json);  // 默认 verify_hash = true
    CHECK(!bad.ok());
    CHECK(bad.status().code() == my::Errc::ParseError);
    CHECK(bad.status().message().find("sha256") != std::string::npos);

    // 明确关掉校验时应当加载成功（模型本身是好的）
    auto relaxed = my::ml::Model::load(tmp_onnx, tmp_json, /*verify_hash=*/false);
    CHECK(relaxed.ok());
}

// sidecar 推导只能在**文件名**里找扩展名：目录名带点时不能拼出 v1.json。
MYTEST_CASE(ml_sidecar_in_dotted_directory) {
    if (!ml_ready()) MYTEST_SKIP("没有可用的推理后端");

    const std::string src_onnx = models_root() + "/ml/iris_logreg.onnx";
    const std::string src_json = models_root() + "/ml/iris_logreg.json";
    auto              onnx_txt = my::io::read_file(src_onnx);
    auto              json_txt = my::io::read_file(src_json);
    CHECK(onnx_txt.ok());
    CHECK(json_txt.ok());
    if (!onnx_txt.ok() || !json_txt.ok()) return;

#if defined(_WIN32)
    const std::string dir =
        std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "\\mylib_ml_v1.0";
#else
    const std::string dir =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/mylib_ml_v1.0";
#endif
    // 文件名故意不带扩展名：旧实现会用目录名里的 ".0" 当扩展名，去找 v1.json
    const std::string net    = dir + "/net";
    const std::string sidecar = net + ".json";
    (void)my::io::create_directories(dir);
    CHECK(my::io::write_file(net, onnx_txt.value()).ok());
    CHECK(my::io::write_file(sidecar, json_txt.value()).ok());

    auto model = my::ml::Model::load(net);
    CHECK_RESULT(model);
    CHECK_EQ(model.value().meta().n_features, 4);
    CHECK_EQ(model.value().meta().classes.size(), std::size_t{3});
    CHECK_EQ(model.value().meta().onnx_sha256.size(), std::size_t{64});

    (void)my::io::remove(net);
    (void)my::io::remove(sidecar);
    (void)my::io::remove_all(dir);
}