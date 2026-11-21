#include "test_framework.hpp"

#include <my_cpp_libs/infer/infer.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

MYTEST_SUITE("infer");

MYTEST_CASE(infer_dtype_helpers) {
    CHECK_EQ(my::infer::dtype_size(my::infer::DType::Float32), std::size_t{4});
    CHECK_EQ(my::infer::dtype_size(my::infer::DType::Int64), std::size_t{8});
    CHECK_EQ(my::infer::dtype_size(my::infer::DType::UInt8), std::size_t{1});
    CHECK_EQ(my::infer::dtype_size(my::infer::DType::Unknown), std::size_t{0});
    CHECK_EQ(std::string(my::infer::dtype_name(my::infer::DType::Float32)), std::string("f32"));
}

MYTEST_CASE(infer_shape) {
    my::infer::Shape s{1, 3, 224, 224};
    CHECK_EQ(s.rank(), std::size_t{4});
    CHECK_EQ(s.elements(), std::int64_t{150528});
    CHECK_EQ(s.to_string(), std::string("[1, 3, 224, 224]"));

    my::infer::Shape dynamic{1, -1, 10};
    CHECK_EQ(dynamic.elements(), std::int64_t{0});

    CHECK_EQ(my::infer::Shape{}.elements(), std::int64_t{0});
}

MYTEST_CASE(infer_tensor_bytes) {
    std::vector<float> data(6, 0.0f);
    my::infer::Tensor  t(my::infer::DType::Float32, my::infer::Shape{1, 2, 3}, data.data());
    CHECK_EQ(t.byte_size(), std::size_t{24});
    CHECK(t.valid());

    my::infer::Tensor empty;
    CHECK(!empty.valid());
    CHECK_EQ(empty.byte_size(), std::size_t{0});
}

MYTEST_CASE(infer_stub_roundtrip) {
    my::infer::Options options;  // model_path 为空 -> stub
    auto session = my::infer::create_session(options);
    CHECK_RESULT(session);

    auto& s = *session.value();
    CHECK_EQ(std::string(s.backend_name()), std::string("stub"));
    CHECK_EQ(s.inputs().size(), std::size_t{1});
    CHECK_EQ(s.outputs().size(), std::size_t{1});

    std::vector<float> input{1.0f, 2.0f, 3.0f, 4.0f};
    auto output = s.run_single_f32(input, my::infer::Shape{1, 1, 4});
    CHECK_RESULT(output);
    CHECK_EQ(output.value().size(), std::size_t{4});
    CHECK_NEAR(output.value()[0], 1.0, 1e-6);
    CHECK_NEAR(output.value()[3], 4.0, 1e-6);
}

MYTEST_CASE(infer_shape_overflow_is_rejected) {
    // 乘法逐维检查：不能触发有符号溢出（UBSan 会 trap）
    my::infer::Shape huge{(std::numeric_limits<std::int64_t>::max)(), 2};
    CHECK_EQ(huge.elements(), std::int64_t{0});

    my::infer::Tensor t(my::infer::DType::Float32, huge, reinterpret_cast<void*>(1));
    CHECK_EQ(t.byte_size(), std::size_t{0});
    CHECK(!t.valid());
}

MYTEST_CASE(infer_run_single_rejects_bad_input) {
    my::infer::Options options;
    auto session = my::infer::create_session(options);
    CHECK_RESULT(session);

    // 空输入
    auto empty = session.value()->run_single_f32({}, my::infer::Shape{1, 1, 4});
    CHECK(!empty);
    CHECK_EQ(empty.status().code(), my::Errc::InvalidArgument);

    // 形状与数据长度不匹配
    auto mismatch = session.value()->run_single_f32({1.0f, 2.0f}, my::infer::Shape{1, 3});
    CHECK(!mismatch);
    CHECK_EQ(mismatch.status().code(), my::Errc::InvalidArgument);

    // 空 Tensor 送给 stub 后端
    std::vector<my::infer::Feed> feeds;
    my::infer::Tensor empty_tensor;
    feeds.push_back(my::infer::Feed{"input", empty_tensor});
    std::vector<my::infer::Outcome> outcomes;
    auto st = session.value()->run(feeds, outcomes);
    CHECK(!st);
    CHECK_EQ(st.code(), my::Errc::InvalidArgument);
}

MYTEST_CASE(infer_stub_rejects_bad_feed) {
    my::infer::Options options;
    auto session = my::infer::create_session(options);
    CHECK_RESULT(session);

    std::vector<my::infer::Feed>  feeds;   // 空
    std::vector<my::infer::Outcome> outcomes;
    auto st = session.value()->run(feeds, outcomes);
    CHECK(!st.ok());
    CHECK_EQ(st.code(), my::Errc::InvalidArgument);
}

MYTEST_CASE(infer_unknown_backend) {
    my::infer::Options options;
    options.backend = "this_backend_does_not_exist";
    auto session = my::infer::create_session(options);
    CHECK(!session.ok());
    CHECK_EQ(session.status().code(), my::Errc::Unsupported);
}

MYTEST_CASE(infer_registry) {
    const auto backends = my::infer::available_backends();
    CHECK(!backends.empty());
    CHECK(my::infer::has_backend("stub"));
    // 没注册的名字要老实返回 false
    CHECK(!my::infer::has_backend("definitely_not_a_backend"));
    CHECK(std::strlen(my::infer::build_backend()) > 0);

    // 编译期后端必须真的在注册表里
    CHECK(my::infer::has_backend(my::infer::build_backend()));
}

// -----------------------------------------------------------------------------
//  张量类型转换
// -----------------------------------------------------------------------------
MYTEST_CASE(infer_tensor_to_float32) {
    // f32
    std::vector<float> f32{1.0f, -2.5f, 3.0f};
    my::infer::Tensor t32(my::infer::DType::Float32, my::infer::Shape{3}, f32.data());
    auto r32 = my::infer::tensor_to_float32(t32);
    CHECK_RESULT(r32);
    CHECK_EQ(r32.value().size(), std::size_t{3});
    CHECK_NEAR(r32.value()[1], -2.5, 1e-9);

    // f16：手动打包 1.0 / 2.0
    std::vector<std::uint16_t> f16{0x3C00, 0x4000};
    my::infer::Tensor t16(my::infer::DType::Float16, my::infer::Shape{2}, f16.data());
    auto r16 = my::infer::tensor_to_float32(t16);
    CHECK_RESULT(r16);
    CHECK_NEAR(r16.value()[0], 1.0, 1e-6);
    CHECK_NEAR(r16.value()[1], 2.0, 1e-6);

    // i64
    std::vector<std::int64_t> i64{7, -9};
    my::infer::Tensor ti64(my::infer::DType::Int64, my::infer::Shape{2}, i64.data());
    auto ri64 = my::infer::tensor_to_float32(ti64);
    CHECK_RESULT(ri64);
    CHECK_NEAR(ri64.value()[0], 7.0, 1e-6);
    CHECK_NEAR(ri64.value()[1], -9.0, 1e-6);

    // u8
    std::vector<std::uint8_t> u8{0, 128, 255};
    my::infer::Tensor tu8(my::infer::DType::UInt8, my::infer::Shape{3}, u8.data());
    auto ru8 = my::infer::tensor_to_float32(tu8);
    CHECK_RESULT(ru8);
    CHECK_NEAR(ru8.value()[2], 255.0, 1e-6);

    // 空张量
    my::infer::Tensor empty;
    CHECK_ERR(my::infer::tensor_to_float32(empty));
}

MYTEST_CASE(infer_tensor_string) {
    std::vector<float> data(6, 0.0f);
    my::infer::Tensor t(my::infer::DType::Float32, my::infer::Shape{1, 2, 3}, data.data());
    const std::string s = my::infer::tensor_to_string(t);
    CHECK(s.find("f32") != std::string::npos);
    CHECK(s.find("[1, 2, 3]") != std::string::npos);
}

// -----------------------------------------------------------------------------
//  模型探查
// -----------------------------------------------------------------------------
MYTEST_CASE(infer_inspect_stub) {
    // 没有模型文件时走 stub，探查应当成功并返回 stub 的端口
    my::infer::Options opt;
    auto info = my::infer::inspect_model(opt);
    CHECK_RESULT(info);
    CHECK_EQ(info.value().backend, std::string("stub"));
    CHECK_EQ(info.value().inputs.size(), std::size_t{1});
    CHECK_EQ(info.value().outputs.size(), std::size_t{1});
}

MYTEST_CASE(infer_inspect_missing_file) {
    my::infer::Options opt;
    opt.model_path = "output/definitely_not_a_model.onnx";
    auto info = my::infer::inspect_model(opt);
    CHECK(!info.ok());
}

// -----------------------------------------------------------------------------
//  后端注册表的边界行为
// -----------------------------------------------------------------------------
// register_backend 的返回类型是 void —— 没有错误码可断言，按 src/infer/infer.cpp
// 的实际实现，能观测到的契约是：
//   * 空名字 / 空 factory 被**静默忽略**（early return，不是错误）；
//   * 同名后注册者覆盖先注册者（header 的约定），顺序表里不留重复项；
//   * 空 factory 连"清掉已有注册"都做不到。
MYTEST_CASE(infer_register_backend_errors) {
    using my::infer::Options;
    using my::infer::Session;
    using my::infer::SessionFactory;

    const std::size_t before = my::infer::available_backends().size();

    // 空名字：什么都不注册，顺序表里也不能出现空串
    my::infer::register_backend("", SessionFactory{});
    CHECK_EQ(my::infer::available_backends().size(), before);
    for (const auto& name : my::infer::available_backends()) {
        CHECK(!name.empty());
    }
    CHECK(!my::infer::has_backend(""));

    // 空 factory：不能注册新名字
    my::infer::register_backend("tests_infer_empty_factory", SessionFactory{});
    CHECK(!my::infer::has_backend("tests_infer_empty_factory"));

    // 同名覆盖：两个 factory 返回不同的错误信息，能看出最终生效的是哪个
    auto make_broken_factory = [](const char* tag) {
        return SessionFactory([tag](const Options&) -> my::Result<std::unique_ptr<Session>> {
            return my::Status(my::Errc::Unsupported, tag);
        });
    };
    my::infer::register_backend("tests_infer_override", make_broken_factory("first"));
    CHECK(my::infer::has_backend("tests_infer_override"));
    my::infer::register_backend("tests_infer_override", make_broken_factory("second"));

    Options opt;
    opt.backend = "tests_infer_override";
    const auto session = my::infer::create_session(opt);
    CHECK(!session.ok());
    CHECK_EQ(session.status().code(), my::Errc::Unsupported);
    CHECK(session.status().message().find("second") != std::string::npos);
    CHECK(session.status().message().find("first") == std::string::npos);

    // 覆盖注册不会在顺序表里留下重复项
    std::size_t occurrences = 0;
    for (const auto& name : my::infer::available_backends()) {
        if (name == "tests_infer_override") ++occurrences;
    }
    CHECK_EQ(occurrences, std::size_t{1});

    // 再拿空 factory 覆盖同名：early return，旧 factory 必须还在
    my::infer::register_backend("tests_infer_override", SessionFactory{});
    CHECK(my::infer::has_backend("tests_infer_override"));
    const auto again = my::infer::create_session(opt);
    CHECK(!again.ok());
    CHECK(again.status().message().find("second") != std::string::npos);
}

// -----------------------------------------------------------------------------
//  动态维 / 大形状
// -----------------------------------------------------------------------------
MYTEST_CASE(infer_shape_dynamic_and_overflow) {
    // 动态维（<0）无法确定元素数 -> 0；to_string 保留原样的 -1
    const my::infer::Shape dyn{-1, 3, 224, 224};
    CHECK_EQ(dyn.rank(), std::size_t{4});
    CHECK_EQ(dyn.elements(), std::int64_t{0});
    CHECK_EQ(dyn.to_string(), std::string("[-1, 3, 224, 224]"));

    // 0 维同样按动态处理（实现是 d <= 0 就返回 0）
    CHECK_EQ((my::infer::Shape{0, 3}.elements()), std::int64_t{0});

    // 动态形状 + 非空指针：字节数无法推算 -> 0，valid() 必须为 false，
    // 转换函数应当返回 InvalidArgument 而不是按 0 元素去解引用
    std::vector<float> dummy(1, 0.0f);
    my::infer::Tensor t(my::infer::DType::Float32, dyn, dummy.data());
    CHECK_EQ(t.byte_size(), std::size_t{0});
    CHECK(!t.valid());
    const auto conv = my::infer::tensor_to_float32(t);
    CHECK(!conv.ok());
    CHECK_EQ(conv.status().code(), my::Errc::InvalidArgument);

    // 显式 bytes 优先于 shape 推算（外部内存 / 带 padding 的场景）
    std::vector<float> data{1.0f, 2.0f, 3.0f, 4.0f};
    my::infer::Tensor explicit_bytes(my::infer::DType::Float32, my::infer::Shape{2, 3},
                                     data.data(), 2 * sizeof(float));
    CHECK_EQ(explicit_bytes.byte_size(), std::size_t{8});
    CHECK(explicit_bytes.valid());
    const auto two = my::infer::tensor_to_float32(explicit_bytes);
    CHECK_RESULT(two);
    CHECK_EQ(two.value().size(), std::size_t{2});
    CHECK_NEAR(two.value()[1], 2.0, 1e-9);

    // 大形状：elements 用 int64 算，byte_size = elements * dtype_size
    const my::infer::Shape big{1024, 1024};  // 2^20 元素
    CHECK_EQ(big.elements(), std::int64_t{1024} * 1024);
    my::infer::Tensor big_f32(my::infer::DType::Float32, big, dummy.data());
    CHECK_EQ(big_f32.byte_size(), (std::size_t{1024} * 1024) * 4);  // 4 MiB
    my::infer::Tensor big_i64(my::infer::DType::Int64, big, dummy.data());
    CHECK_EQ(big_i64.byte_size(), (std::size_t{1024} * 1024) * 8);  // 8 MiB

    // elements()/byte_size() 现在逐维检查溢出，溢出时返回 0；这里不构造有符号溢出
    // （那是 UB，UBSan trap 构建会直接崩），对应防护在 infer_shape_overflow_is_rejected。
}

// -----------------------------------------------------------------------------
//  真实 ORT 后端的端口形态回归（docs/04 §3 的 7 个模型）
//
//  这些模型以前只用 tools/model_inspect 手工验证过，测试里没有；ORT 改名/升级
//  （1.23 的 API 变化）时不会有任何用例变红。这里用 Session 的 Port 信息自动
//  构造输入，逐个覆盖 f32/i64/f16/u8、动态 batch、2 进 2 出、CNN 和 softmax。
//  没有 ORT 后端（MinGW/Linux 未接入）时整个用例 SKIP。
// -----------------------------------------------------------------------------
namespace {

const char* models_root() {
#ifdef MYLIB_TEST_MODELS_DIR
    return MYLIB_TEST_MODELS_DIR;
#else
    return "models";
#endif
}

struct ModelCase {
    const char* file;
    bool        dynamic_batch;  ///< 是否需要把 -1 维替换成 1
};

bool run_ort_model(const ModelCase& mc) {
    using namespace my::infer;

    Options opt;
    opt.backend    = "onnxruntime";
    opt.model_path = std::string(models_root()) + "/" + mc.file;

    auto made = create_session(opt);
    if (!made) {
        std::printf("         模型 %s 创建 Session 失败: %s\n", mc.file,
                    made.status().to_string().c_str());
        return false;
    }
    Session& s = *made.value();

    const auto& in_ports  = s.inputs();
    const auto& out_ports = s.outputs();
    if (in_ports.empty() || out_ports.empty()) return false;

    std::vector<std::vector<std::uint8_t>> buffers;
    std::vector<Feed>                      feeds;
    buffers.reserve(in_ports.size());
    feeds.reserve(in_ports.size());

    for (const auto& port : in_ports) {
        Shape shape = port.shape;
        if (mc.dynamic_batch) {
            for (auto& d : shape.dims) {
                if (d <= 0) d = 1;
            }
        }
        const std::int64_t elems = shape.elements();
        if (elems <= 0) return false;
        const std::size_t bytes = static_cast<std::size_t>(elems) * dtype_size(port.type);
        if (bytes == 0) return false;

        buffers.emplace_back(bytes, 0);  // 全 0 输入只验证端口/类型/执行，不验证数值语义
        feeds.push_back(Feed{port.name, Tensor(port.type, shape, buffers.back().data(), bytes)});
    }

    std::vector<Outcome> outs;
    auto                 st = s.run(feeds, outs);
    if (!st) {
        std::printf("         模型 %s run 失败: %s\n", mc.file, st.to_string().c_str());
        return false;
    }
    if (outs.size() != out_ports.size()) return false;

    for (std::size_t i = 0; i < outs.size(); ++i) {
        const Outcome& o = outs[i];
        if (!o.tensor.valid()) return false;
        if (o.tensor.type != out_ports[i].type) return false;
        // 非 f16/int 的常见输出都转 float 检查有限性；转换失败直接判错
        const auto conv = tensor_to_float32(o.tensor);
        if (!conv) return false;
        for (float v : conv.value()) {
            if (!std::isfinite(v)) return false;
        }
    }
    return true;
}

}  // namespace

MYTEST_CASE(infer_ort_model_port_forms) {
    if (!my::infer::has_backend("onnxruntime")) {
        MYTEST_SKIP("未编译 ONNX Runtime 后端");
    }

    const ModelCase cases[] = {
        {"dyn_batch.onnx", true},   {"multi_io.onnx", false},
        {"int64_input.onnx", false}, {"fp16_io.onnx", false},
        {"u8_input.onnx", false},   {"small_cnn.onnx", false},
        {"softmax_1d.onnx", false},
    };
    for (const auto& mc : cases) {
        CHECK(run_ort_model(mc));
    }

    // softmax_1d 的全 0 输入应输出均匀 0.25；单独再跑一次验证数值路径
    my::infer::Options opt;
    opt.backend    = "onnxruntime";
    opt.model_path = std::string(models_root()) + "/softmax_1d.onnx";
    auto made      = my::infer::create_session(opt);
    CHECK_RESULT(made);
    std::vector<float> input{0.0f, 0.0f, 0.0f, 0.0f};
    auto out = made.value()->run_single_f32(input, my::infer::Shape{4});
    CHECK_RESULT(out);
    CHECK_EQ(out.value().size(), std::size_t{4});
    for (float v : out.value()) CHECK_NEAR(v, 0.25, 1e-6);
}

MYTEST_CASE(infer_ncnn_model_port_forms) {
    if (!my::infer::has_backend("ncnn")) {
        MYTEST_SKIP("未编译 ncnn 后端");
    }

    // ncnn 的 .param 才是入口；create_session 用它推导 .bin
    my::infer::Options opt;
    opt.backend    = "ncnn";
    opt.model_path = std::string(models_root()) + "/ncnn/small_cnn.param";
    auto made      = my::infer::create_session(opt);
    CHECK_RESULT(made);

    const auto& in = made.value()->inputs();
    CHECK(!in.empty());
    if (in.empty()) return;

    // ncnn 的 Port.shape 是空的（网络输入 blob 不带静态形状），按模型约定给 1x3x32x32
    const my::infer::Shape shape{1, 3, 32, 32};
    const std::int64_t     elems = shape.elements();
    CHECK(elems > 0);
    std::vector<float> input(static_cast<std::size_t>(elems), 0.0f);
    std::vector<my::infer::Feed> feeds;
    feeds.push_back(my::infer::Feed{in[0].name,
                                    my::infer::Tensor(my::infer::DType::Float32, shape,
                                                      input.data(), input.size() * sizeof(float))});
    std::vector<my::infer::Outcome> outs;
    CHECK_OK(made.value()->run(feeds, outs));
    CHECK(!outs.empty());
    for (const auto& o : outs) CHECK(o.tensor.valid());
}
