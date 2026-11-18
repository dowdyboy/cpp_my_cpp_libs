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

    // 注意：elements() 的 int64 乘积、byte_size() 的 size_t 乘积都没有溢出检查。
    // 这里刻意不构造会让 int64 有符号溢出的形状（那是 UB，UBSan trap 构建会直接崩），
    // "溢出没有防护"这一点写在测试报告里，而不是锁进断言。
}
