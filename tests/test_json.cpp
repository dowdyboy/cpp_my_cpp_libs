#include "test_framework.hpp"

#include <my_cpp_libs/json/json.hpp>

#include <filesystem>
#include <string>

using my::json::Document;
using my::json::Type;

MYTEST_CASE(json_parse_and_type) {
    auto r = Document::parse(R"({"a":1,"b":"x","c":[1,2],"d":null,"e":true,"f":1.5})");
    CHECK(r.ok());
    const auto& d = r.value();
    CHECK(d.is_object());
    CHECK_EQ(d.size(), std::size_t{6});
    CHECK(d.contains("a"));
    CHECK(!d.contains("zzz"));
    CHECK_EQ(static_cast<int>(d.type_of("a")), static_cast<int>(Type::Integer));
    CHECK_EQ(static_cast<int>(d.type_of("b")), static_cast<int>(Type::String));
    CHECK_EQ(static_cast<int>(d.type_of("c")), static_cast<int>(Type::Array));
    CHECK_EQ(static_cast<int>(d.type_of("d")), static_cast<int>(Type::Null));
    CHECK_EQ(static_cast<int>(d.type_of("e")), static_cast<int>(Type::Boolean));
    CHECK_EQ(static_cast<int>(d.type_of("f")), static_cast<int>(Type::Floating));
    CHECK_EQ(static_cast<int>(d.type_of("missing")), static_cast<int>(Type::Unknown));
}

MYTEST_CASE(json_parse_error) {
    auto r = Document::parse("{ not json }");
    CHECK(!r.ok());
    CHECK_EQ(r.status().code(), my::Errc::ParseError);

    auto empty = Document::parse("");
    CHECK(!empty.ok());
}

MYTEST_CASE(json_path_get) {
    auto r = Document::parse(R"({"a":{"b":{"c":42}},"arr":[10,20,30]})");
    CHECK(r.ok());
    const auto& d = r.value();

    CHECK_EQ(d.get<int>("a.b.c").value(), 42);
    CHECK_EQ(d.get<int>("arr[0]").value(), 10);
    CHECK_EQ(d.get<int>("arr[2]").value(), 30);

    // 越界 / 不存在 / 类型错误
    CHECK_EQ(d.get<int>("arr[5]").status().code(), my::Errc::NotFound);
    CHECK_EQ(d.get<int>("a.x").status().code(), my::Errc::NotFound);
    CHECK_EQ(d.get<int>("a.b.c.deeper").status().code(), my::Errc::NotFound);
    CHECK_EQ(d.get<std::string>("a.b.c").status().code(), my::Errc::ParseError);

    // 路径语法错误
    CHECK_EQ(d.get<int>("a..b").status().code(), my::Errc::InvalidArgument);
    CHECK_EQ(d.get<int>("arr[").status().code(), my::Errc::InvalidArgument);
    CHECK_EQ(d.get<int>("arr[x]").status().code(), my::Errc::InvalidArgument);
}

MYTEST_CASE(json_path_set) {
    Document d;
    CHECK_OK(d.set("a.b.c", 1));
    CHECK_OK(d.set("arr[0]", "x"));
    CHECK_OK(d.set("arr[2]", "z"));
    CHECK_OK(d.set("num", 3.5));
    CHECK_OK(d.set("flag", true));

    CHECK_EQ(d.get<int>("a.b.c").value(), 1);
    CHECK_EQ(d.get<std::string>("arr[0]").value(), std::string("x"));
    CHECK_EQ(d.get<std::string>("arr[2]").value(), std::string("z"));
    CHECK_NEAR(d.get<double>("num").value(), 3.5, 1e-9);
    CHECK(d.get<bool>("flag").value());

    // 数组中间补 null
    CHECK_EQ(d.size("arr"), std::size_t{3});

    // 覆盖已有值
    CHECK_OK(d.set("a.b.c", 99));
    CHECK_EQ(d.get<int>("a.b.c").value(), 99);
}

MYTEST_CASE(json_get_or) {
    auto r = Document::parse(R"({"x":1})");
    CHECK(r.ok());
    CHECK_EQ(r.value().get_or<int>("x", 0), 1);
    CHECK_EQ(r.value().get_or<int>("y", -1), -1);
    CHECK_EQ(r.value().get_or<int>("x.deep", -2), -2);
}

MYTEST_CASE(json_erase_and_keys) {
    auto r = Document::parse(R"({"a":1,"b":2,"arr":[1,2,3]})");
    CHECK(r.ok());
    auto& d = r.value();

    auto keys = d.keys();
    CHECK_EQ(keys.size(), std::size_t{3});

    CHECK_OK(d.erase("a"));
    CHECK(!d.contains("a"));
    CHECK_EQ(d.keys().size(), std::size_t{2});

    CHECK_OK(d.erase("arr[1]"));
    CHECK_EQ(d.size("arr"), std::size_t{2});

    CHECK_EQ(d.erase("nope").code(), my::Errc::NotFound);
    CHECK_EQ(d.erase("arr[99]").code(), my::Errc::NotFound);
}

MYTEST_CASE(json_dump_roundtrip) {
    auto r = Document::parse(R"({"n":1,"s":"中文","f":0.25,"arr":[1,2]})");
    CHECK(r.ok());
    const std::string text = r.value().dump();
    auto again = Document::parse(text);
    CHECK(again.ok());
    CHECK_EQ(again.value().get<int>("n").value(), 1);
    CHECK_EQ(again.value().get<std::string>("s").value(), std::string("中文"));
}

MYTEST_CASE(json_file_roundtrip) {
    std::error_code ec;
    std::filesystem::create_directories("output", ec);

    Document d;
    CHECK_OK(d.set("hello.world", 1));
    CHECK_OK(d.save_file("output/test_json.json", 2));

    auto loaded = Document::parse_file("output/test_json.json");
    CHECK(loaded.ok());
    CHECK_EQ(loaded.value().get<int>("hello.world").value(), 1);

    auto missing = Document::parse_file("output/definitely_not_here.json");
    CHECK(!missing.ok());
    CHECK_EQ(missing.status().code(), my::Errc::IoError);
}

MYTEST_CASE(json_struct) {
    struct Config {
        int         width = 0;
        std::string name;
    };
    // 手工映射（不依赖宏，验证 escape hatch）
    auto r = Document::parse(R"({"cam":{"width":640,"name":"front"}})");
    CHECK(r.ok());

    my::json::Json& raw = r.value().raw();
    const auto&     cam = raw["cam"];
    Config          cfg;
    cfg.width = cam.at("width").get<int>();
    cfg.name  = cam.at("name").get<std::string>();

    CHECK_EQ(cfg.width, 640);
    CHECK_EQ(cfg.name, std::string("front"));
}
