#include "test_framework.hpp"

#include <my_cpp_libs/json/json.hpp>

#include <filesystem>
#include <string>

using my::json::Document;
using my::json::Type;

MYTEST_SUITE("json");

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

    CHECK_OK_VALUE(v44, d.get<int>("a.b.c"));
    CHECK_EQ(v44, 42);
    CHECK_OK_VALUE(a44, d.get<int>("arr[0]"));
    CHECK_EQ(a44, 10);
    CHECK_OK_VALUE(a46, d.get<int>("arr[2]"));
    CHECK_EQ(a46, 30);

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

    CHECK_OK_VALUE(c68, d.get<int>("a.b.c"));
    CHECK_EQ(c68, 1);
    CHECK_OK_VALUE(s69, d.get<std::string>("arr[0]"));
    CHECK_EQ(s69, std::string("x"));
    CHECK_OK_VALUE(s70, d.get<std::string>("arr[2]"));
    CHECK_EQ(s70, std::string("z"));
    CHECK_OK_VALUE(n71, d.get<double>("num"));
    CHECK_NEAR(n71, 3.5, 1e-9);
    CHECK_OK_VALUE(b72, d.get<bool>("flag"));
    CHECK(b72);

    // 数组中间补 null
    CHECK_EQ(d.size("arr"), std::size_t{3});

    // 覆盖已有值
    CHECK_OK(d.set("a.b.c", 99));
    CHECK_OK_VALUE(v79, d.get<int>("a.b.c"));
    CHECK_EQ(v79, 99);
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
    CHECK_OK_VALUE(n115, again.value().get<int>("n"));
    CHECK_EQ(n115, 1);
    CHECK_OK_VALUE(s116, again.value().get<std::string>("s"));
    CHECK_EQ(s116, std::string("中文"));
}

MYTEST_CASE(json_file_roundtrip) {
    std::error_code ec;
    std::filesystem::create_directories("output", ec);

    Document d;
    CHECK_OK(d.set("hello.world", 1));
    CHECK_OK(d.save_file("output/test_json.json", 2));

    auto loaded = Document::parse_file("output/test_json.json");
    CHECK(loaded.ok());
    CHECK_OK_VALUE(v129, loaded.value().get<int>("hello.world"));
    CHECK_EQ(v129, 1);

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

// -----------------------------------------------------------------------------
//  路径解析的安全边界（输入放大 / size_t 回绕）
// -----------------------------------------------------------------------------
// 回归：set("a[N]") 会把 0..N-1 全部补 null，N 来自配置文件/网络报文时
// 一个 a[1000000000] 就能吃掉几 GB —— 属于"输入放大"问题。
// 现在写路径按 kMaxSparseIndex=65536 拒绝（在 resolve_create 里，因为读路径 a[500000] 合法）。
MYTEST_CASE(json_set_huge_index_rejected) {
    Document   d;
    const auto st = d.set("a[99999999]", 1);
    CHECK(!st.ok());
    CHECK_EQ(st.code(), my::Errc::OutOfRange);

    // 关键：绝不能把 0..N-1 补成 null，否则一次 set 就是 GB 级放大。
    // （当前实现在拒绝前会先把父容器 a 建成空数组，所以这里只查"数组里没有元素"，
    //   不锁死"整个文档一个字节都没动"；这一点写在测试报告里。）
    CHECK(d.dump().size() < 1024);
    CHECK_EQ(d.size("a"), std::size_t{0});

    // 同样的超大下标出现在路径中间也一样
    const auto st2 = d.set("x.y[99999999]", 2);
    CHECK(!st2.ok());
    CHECK_EQ(st2.code(), my::Errc::OutOfRange);
    CHECK_EQ(d.size("x.y"), std::size_t{0});
    CHECK(d.dump().size() < 1024);
}

// 超长十进制数串：逐个数字累积时若不设上限会回绕成一个小下标，
// 于是"非法路径"变成"合法的小数组下标"。读路径也必须拒绝，不能退化成 NotFound。
MYTEST_CASE(json_path_index_overflow_rejected) {
    Document d;

    const auto set_huge = d.set("a[999999999999999999999]", 1);
    CHECK(!set_huge.ok());
    CHECK_EQ(set_huge.code(), my::Errc::OutOfRange);
    CHECK(d.dump().size() < 1024);

    const auto get_huge = d.get<int>("a[999999999999999999999]");
    CHECK(!get_huge.ok());
    CHECK_EQ(get_huge.status().code(), my::Errc::OutOfRange);

    // 边界：65536 是允许的上限（含），65537 越界
    CHECK_OK(d.set("b[65536]", 1));
    CHECK_EQ(d.size("b"), std::size_t{65537});
    CHECK_OK_VALUE(v, d.get<int>("b[65536]"));
    CHECK_EQ(v, 1);

    const auto over = d.set("b[65537]", 2);
    CHECK(!over.ok());
    CHECK_EQ(over.code(), my::Errc::OutOfRange);
    CHECK_EQ(d.size("b"), std::size_t{65537});  // 失败不能改动已有数组
}

// 路径语法错误一律 InvalidArgument（parse_path 的实际规则）：
// 缺右括号 / 空索引 / 负索引 / 前导点 / 尾随点 / 连续点 / 非数字索引。
MYTEST_CASE(json_path_malformed_rejected) {
    Document d;
    CHECK_OK(d.set("a", 1));

    const char* const bad_paths[] = {"a[", "a[]", "a[-1]", ".a", "a.", "a..b", "a[x]"};
    for (const char* p : bad_paths) {
        const auto got = d.get<int>(p);
        CHECK(!got.ok());
        CHECK_EQ(got.status().code(), my::Errc::InvalidArgument);

        const auto put = d.set(p, 2);
        CHECK(!put.ok());
        CHECK_EQ(put.code(), my::Errc::InvalidArgument);
    }

    // 全部失败之后文档没有被改坏
    CHECK_OK_VALUE(v, d.get<int>("a"));
    CHECK_EQ(v, 1);
    CHECK_EQ(d.keys().size(), std::size_t{1});
}

// 类型冲突：标量不能当对象/数组继续下钻。必须报错，不能静默把 1 覆盖成容器。
MYTEST_CASE(json_path_type_conflict) {
    Document d;
    CHECK_OK(d.set("a", 1));

    const auto st = d.set("a.b", 2);
    CHECK(!st.ok());
    CHECK_EQ(st.code(), my::Errc::InvalidArgument);
    CHECK_EQ(d.type_of("a"), Type::Integer);
    CHECK_OK_VALUE(v1, d.get<int>("a"));
    CHECK_EQ(v1, 1);

    // 反过来：对象不能让 [N] 下钻
    Document e;
    CHECK_OK(e.set("a.b", 1));

    const auto st2 = e.set("a[0]", 2);
    CHECK(!st2.ok());
    CHECK_EQ(st2.code(), my::Errc::InvalidArgument);
    CHECK_EQ(e.type_of("a"), Type::Object);
    CHECK_EQ(e.size("a"), std::size_t{1});  // 仍是 {b:1}，没被改成数组
    CHECK_OK_VALUE(v2, e.get<int>("a.b"));
    CHECK_EQ(v2, 1);
}

// set 按路径形态自动创建中间容器，且类型必须与路径一致。
// 先验证当前能走通的形态（下标在路径末尾）：a 对象 -> b 数组 -> [0] 整数。
MYTEST_CASE(json_set_creates_nested_containers) {
    Document d;
    CHECK_OK(d.set("a.b[0]", 42));
    CHECK_EQ(d.type_of("a"), Type::Object);
    CHECK_EQ(d.type_of("a.b"), Type::Array);
    CHECK_EQ(d.size("a.b"), std::size_t{1});
    CHECK_EQ(d.type_of("a.b[0]"), Type::Integer);
    CHECK_OK_VALUE(v, d.get<int>("a.b[0]"));
    CHECK_EQ(v, 42);

    // 追加第二个元素：已有元素必须保留
    CHECK_OK(d.set("a.b[1]", 43));
    CHECK_EQ(d.size("a.b"), std::size_t{2});
    CHECK_OK_VALUE(v0, d.get<int>("a.b[0]"));
    CHECK_EQ(v0, 42);
    CHECK_OK_VALUE(v1, d.get<int>("a.b[1]"));
    CHECK_EQ(v1, 43);

    // 下标后面还能继续下钻：a=对象、a.b=数组、a.b[0]=对象、a.b[0].c=整数。
    // 这条是头文件注释声明的语法（`a.b[2].c`）；0.1.1 修掉了"']' 后紧跟 '.' 被误判成
    // 空键"的缺陷，所以这里必须**无条件**断言，不能再 SKIP。
    Document e;
    CHECK_OK(e.set("a.b[0].c", 42));
    CHECK_OK_VALUE(c, e.get<int>("a.b[0].c"));
    CHECK_EQ(c, 42);
    CHECK_EQ(e.type_of("a.b[0]"), Type::Object);
    CHECK_EQ(e.type_of("a.b[0].c"), Type::Integer);
}

// set() 失败不能留下已创建的中间节点（错误返回 = 文档未变）。
MYTEST_CASE(json_set_failure_is_strong) {
    Document d;
    CHECK_OK(d.set("keep", 1));

    auto st = d.set("a.b[1000000000]", 2);
    CHECK(!st);
    CHECK_EQ(st.code(), my::Errc::OutOfRange);

    CHECK_EQ(d.type_of("a"), my::json::Type::Unknown);
    CHECK(!d.contains("a"));
    CHECK_OK_VALUE(keep, d.get<int>("keep"));
    CHECK_EQ(keep, 1);

    // 父节点类型冲突同样不能改坏原文档
    CHECK_OK(d.set("x", std::string("str")));
    auto conflict = d.set("x.y", 3);
    CHECK(!conflict);
    CHECK_EQ(d.type_of("x"), my::json::Type::String);
    CHECK_OK_VALUE(x, d.get<std::string>("x"));
    CHECK_EQ(x, std::string("str"));
}

