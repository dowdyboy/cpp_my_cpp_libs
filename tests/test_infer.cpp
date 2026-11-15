#include "test_framework.hpp"

#include <my_cpp_libs/infer/infer.hpp>

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