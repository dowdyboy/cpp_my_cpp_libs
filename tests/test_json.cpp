#include "test_framework.hpp"

#include <my_cpp_libs/json/config.hpp>
#include <my_cpp_libs/json/json.hpp>

#include <cstdint>
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

// -----------------------------------------------------------------------------
//  A11：深层嵌套在 parse 入口按词法深度上限拒绝（避免递归解析器爆栈）
// -----------------------------------------------------------------------------
// nlohmann 是递归下降解析器：`[`×15000 的 30 KB 输入会让进程以 0xC00000FD
// （栈溢出）直接死掉，Result/异常都接不住。现在 parse 先做一遍纯词法扫描，
// 深度上限 kMaxNestingDepth=256；扫描必须正确跳过字符串与转义。
MYTEST_CASE(json_rejects_deep_nesting) {
    const auto make_nested = [](std::size_t depth) {
        std::string text;
        text.reserve(depth * 2);
        text.append(depth, '[');
        text.append(depth, ']');
        return text;
    };

    // 100 层在 256 上限内：正常解析
    const auto shallow = Document::parse(make_nested(100));
    CHECK(shallow.ok());
    CHECK(shallow.value().is_array());

    // 300 层超限：必须是 ParseError（不是进程崩溃），消息说明深度上限
    const auto too_deep = Document::parse(make_nested(300));
    CHECK(!too_deep.ok());
    CHECK_EQ(too_deep.status().code(), my::Errc::ParseError);
    CHECK(too_deep.status().message().find("深度") != std::string::npos);
    CHECK(too_deep.status().message().find("256") != std::string::npos);

    // 字符串里的括号不能计入深度：s 里放 300 个 '['，整体仍然只有 2 层结构；
    // q 在 `\"`（转义引号）后再放 300 个 '['：扫描器若把转义引号当成字符串结束，
    // 这 300 个 '[' 就会被误计成嵌套超限。
    const std::string brackets(300, '[');
    std::string       text = "{\"s\":\"";
    text += brackets;
    text += "\",\"q\":\"a\\\"";  // 追加 `","q":"a\"`，其中 \" 是 JSON 的转义引号
    text += brackets;
    text += "\",\"arr\":[[[1]]]}";
    const auto doc = Document::parse(text);
    CHECK(doc.ok());

    CHECK_OK_VALUE(s, doc.value().get<std::string>("s"));
    CHECK_EQ(s, brackets);
    CHECK_OK_VALUE(q, doc.value().get<std::string>("q"));
    CHECK_EQ(q, std::string("a\"") + brackets);
    CHECK_OK_VALUE(inner, doc.value().get<int>("arr[0][0][0]"));
    CHECK_EQ(inner, 1);
}

// -----------------------------------------------------------------------------
//  A11 兜底：逃生舱（Document(Json)/raw()）可以造出超深 Json，merge_into 的
//  递归也要有深度上限，超限返回 RuntimeError（parse 出来的文档不会走到这里）。
// -----------------------------------------------------------------------------
MYTEST_CASE(json_merge_depth_guard) {
    // 构造 depth 层 `{"a":{"a":...}}`；每层都是对象，保证 merge_json 会一直递归
    const auto make_deep = [](std::size_t depth) {
        my::json::Json node = my::json::Json::object();
        for (std::size_t i = 0; i < depth; ++i) {
            my::json::Json parent = my::json::Json::object();
            parent["a"]            = std::move(node);
            node                   = std::move(parent);
        }
        return node;
    };

    Document base(make_deep(300));
    Document overlay(make_deep(300));
    const auto st = my::json::merge_into(base, overlay);
    CHECK(!st.ok());
    CHECK_EQ(st.code(), my::Errc::RuntimeError);
    CHECK(st.message().find("深度") != std::string::npos);

    // 正常深度照常合并
    Document ok_base;
    CHECK_OK(ok_base.set("a.b", 1));
    Document ok_overlay;
    CHECK_OK(ok_overlay.set("a.c", 2));
    CHECK_OK(my::json::merge_into(ok_base, ok_overlay));
    CHECK_OK_VALUE(b, ok_base.get<int>("a.b"));
    CHECK_EQ(b, 1);
    CHECK_OK_VALUE(c, ok_base.get<int>("a.c"));
    CHECK_EQ(c, 2);
}

// -----------------------------------------------------------------------------
//  A12：get<T> 的整型范围检查（越界/非整值返回 ParseError，不静默截断）
// -----------------------------------------------------------------------------
MYTEST_CASE(json_get_integer_range_checked) {
    const auto r = Document::parse(R"({
        "big": 70000,
        "u32": 4294967296,
        "huge": 1e300,
        "frac": 1.9,
        "answer": 42,
        "whole": 42.0,
        "neg": -1,
        "imax": 2147483647,
        "imin": -2147483648,
        "u8max": 255,
        "u8over": 256,
        "three": 3
    })");
    CHECK(r.ok());
    const auto& d = r.value();

    // 超范围：unsigned short 只能到 65535（原实现会静默回绕成 4464）
    const auto big = d.get<unsigned short>("big");
    CHECK(!big.ok());
    CHECK_EQ(big.status().code(), my::Errc::ParseError);
    CHECK(big.status().message().find("big") != std::string::npos);      // 路径
    CHECK(big.status().message().find("65535") != std::string::npos);    // 可接受范围

    // int 放不下 2^32（原实现回绕成 0）
    const auto u32 = d.get<int>("u32");
    CHECK(!u32.ok());
    CHECK_EQ(u32.status().code(), my::Errc::ParseError);

    // 1e300 转 int 是 UB（原实现给出 INT_MIN 之类的结果）
    const auto huge = d.get<int>("huge");
    CHECK(!huge.ok());
    CHECK_EQ(huge.status().code(), my::Errc::ParseError);

    // 非整值浮点不能静默截断成 1
    const auto frac = d.get<int>("frac");
    CHECK(!frac.ok());
    CHECK_EQ(frac.status().code(), my::Errc::ParseError);

    // -1 不能转成无符号（原实现回绕成 UINT64_MAX）
    const auto neg = d.get<std::uint64_t>("neg");
    CHECK(!neg.ok());
    CHECK_EQ(neg.status().code(), my::Errc::ParseError);

    // 正常值与整值浮点
    CHECK_OK_VALUE(answer, d.get<int>("answer"));
    CHECK_EQ(answer, 42);
    CHECK_OK_VALUE(whole, d.get<int>("whole"));
    CHECK_EQ(whole, 42);
    CHECK_OK_VALUE(three, d.get<double>("three"));
    CHECK_NEAR(three, 3.0, 1e-12);

    // int 的边界值必须通过
    CHECK_OK_VALUE(imax, d.get<int>("imax"));
    CHECK_EQ(imax, 2147483647);
    CHECK_OK_VALUE(imin, d.get<int>("imin"));
    CHECK_EQ(imin, -2147483647 - 1);

    // char/unsigned char 也算整型，同样做范围检查
    CHECK_OK_VALUE(u8max, d.get<unsigned char>("u8max"));
    CHECK_EQ(u8max, static_cast<unsigned char>(255));
    const auto u8over = d.get<unsigned char>("u8over");
    CHECK(!u8over.ok());
    CHECK_EQ(u8over.status().code(), my::Errc::ParseError);
}

