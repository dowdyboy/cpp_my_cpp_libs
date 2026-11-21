// =============================================================================
//  分层配置（json/config.hpp）：深合并 / 文件 / 环境变量 / 命令行
// =============================================================================
#include "my_cpp_libs/json/config.hpp"

#include <cstdlib>
#include <fstream>
#include <string>

#include "test_framework.hpp"

MYTEST_SUITE("config");

namespace {

/// 跨平台的 setenv/unsetenv
void set_env(const char* name, const char* value) {
#if defined(_WIN32)
    if (value == nullptr || *value == '\0') {
        _putenv_s(name, "");
    } else {
        _putenv_s(name, value);
    }
#else
    if (value == nullptr || *value == '\0') {
        unsetenv(name);
    } else {
        setenv(name, value, 1);
    }
#endif
}

std::string write_temp_json(const std::string& name, const std::string& text) {
    std::ofstream out(name, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    return name;
}

struct FileGuard {
    std::string path;
    ~FileGuard() { std::remove(path.c_str()); }
};

}  // namespace

MYTEST_CASE(config_defaults_only) {
    my::json::Document defaults;
    CHECK_OK(defaults.set("server.port", 8080));
    CHECK_OK(defaults.set("server.host", "0.0.0.0"));

    my::json::Layered cfg;
    CHECK_OK(cfg.add_defaults(defaults));

    CHECK_EQ(cfg.value<int>("server.port", -1), 8080);
    CHECK_EQ(cfg.value<std::string>("server.host", ""), std::string("0.0.0.0"));
    CHECK_EQ(cfg.value<int>("missing.key", -1), -1);  // 缺失走 fallback
    CHECK(cfg.has("server.port"));
    CHECK(!cfg.has("missing.key"));
    CHECK_EQ(cfg.layers().size(), std::size_t{1});
    CHECK(cfg.layers()[0].kind == my::json::ConfigLayer::Kind::Defaults);
}

MYTEST_CASE(config_deep_merge_preserves_siblings) {
    my::json::Document base;
    CHECK_OK(base.set("a.b.c", 1));
    CHECK_OK(base.set("a.b.d", 2));
    CHECK_OK(base.set("a.e", 3));

    my::json::Document overlay;
    CHECK_OK(overlay.set("a.b.c", 99));   // 只覆盖这一层

    CHECK_OK(my::json::merge_into(base, overlay));
    CHECK_EQ(base.get_or<int>("a.b.c", 0), 99);
    CHECK_EQ(base.get_or<int>("a.b.d", 0), 2);  // 兄弟键必须还在
    CHECK_EQ(base.get_or<int>("a.e", 0), 3);
}

MYTEST_CASE(config_array_is_replaced_wholesale) {
    my::json::Document base;
    CHECK_OK(base.set("list[0]", 1));
    CHECK_OK(base.set("list[1]", 2));
    CHECK_OK(base.set("list[2]", 3));

    my::json::Document overlay;
    CHECK_OK(overlay.set("list[0]", 9));

    CHECK_OK(my::json::merge_into(base, overlay));
    // 数组整体替换：最后只剩 overlay 那一个元素
    CHECK_EQ(base.size("list"), std::size_t{1});
    CHECK_EQ(base.get_or<int>("list[0]", -1), 9);
}

MYTEST_CASE(config_file_overrides_defaults) {
    const std::string path  = "mylib_cfg_file_order.json";
    FileGuard         guard{path};
    write_temp_json(path, R"({"server":{"port":9090}})");

    my::json::Document defaults;
    CHECK_OK(defaults.set("server.port", 8080));
    CHECK_OK(defaults.set("server.host", "127.0.0.1"));

    my::json::Layered cfg;
    CHECK_OK(cfg.add_defaults(defaults));
    CHECK_OK(cfg.add_file(path));

    CHECK_EQ(cfg.value<int>("server.port", -1), 9090);            // 文件覆盖
    CHECK_EQ(cfg.value<std::string>("server.host", ""),           // 文件没写的保留默认
             std::string("127.0.0.1"));
    CHECK_EQ(cfg.layers().size(), std::size_t{2});
}

MYTEST_CASE(config_missing_file_can_be_skipped) {
    my::json::Layered cfg;
    // add_file_if_exists：不存在 → 成功且不产生层
    CHECK_OK(cfg.add_file_if_exists("mylib_cfg_no_such_file.json"));
    CHECK_EQ(cfg.layers().size(), std::size_t{0});

    // add_file：不存在 → 报错（调用方要能区分"没配"和"配错了"）
    auto st = cfg.add_file("mylib_cfg_no_such_file.json");
    CHECK(!st);
}

MYTEST_CASE(config_broken_file_reports_error) {
    const std::string path  = "mylib_cfg_broken.json";
    FileGuard         guard{path};
    write_temp_json(path, R"({"server": {"port": })");  // 故意写坏

    my::json::Layered cfg;
    auto              st = cfg.add_file(path);
    CHECK(!st);
    CHECK_EQ(st.code(), my::Errc::ParseError);
}

MYTEST_CASE(config_env_mapping) {
    set_env("MYLIBCFG_T__SERVER__PORT", "9090");
    set_env("MYLIBCFG_T__SERVER__HOST", "abc");
    set_env("MYLIBCFG_T__DEBUG", "true");

    my::json::Document doc;
    const auto         st = my::json::document_from_env("MYLIBCFG_T", doc);
    set_env("MYLIBCFG_T__SERVER__PORT", nullptr);
    set_env("MYLIBCFG_T__SERVER__HOST", nullptr);
    set_env("MYLIBCFG_T__DEBUG", nullptr);
    CHECK_OK(st);

    CHECK_EQ(doc.get_or<int>("server.port", -1), 9090);        // 数字按 JSON 解析
    CHECK_EQ(doc.get_or<std::string>("server.host", ""), "abc");  // 非 JSON 当字符串
    CHECK_EQ(doc.get_or<bool>("debug", false), true);
}

MYTEST_CASE(config_env_overrides_file) {
    set_env("MYLIBCFG_O__WORKER__COUNT", "8");

    my::json::Document defaults;
    CHECK_OK(defaults.set("worker.count", 2));

    my::json::Layered cfg;
    CHECK_OK(cfg.add_defaults(defaults));
    CHECK_OK(cfg.add_env("MYLIBCFG_O"));
    set_env("MYLIBCFG_O__WORKER__COUNT", nullptr);

    CHECK_EQ(cfg.value<int>("worker.count", -1), 8);
}

MYTEST_CASE(config_argv_forms) {
    const char* argv[] = {"prog", "--server.port=9090", "--server.host", "0.0.0.0", "--verbose",
                          nullptr};
    const int argc = 5;

    my::json::Document doc;
    CHECK_OK(my::json::document_from_argv(argc, argv, doc));

    CHECK_EQ(doc.get_or<int>("server.port", -1), 9090);
    CHECK_EQ(doc.get_or<std::string>("server.host", ""), std::string("0.0.0.0"));
    CHECK_EQ(doc.get_or<bool>("verbose", false), true);
}

MYTEST_CASE(config_argv_overrides_env) {
    set_env("MYLIBCFG_A__PORT", "1111");
    const char* argv[] = {"prog", "--port=2222", nullptr};

    my::json::Layered cfg;
    CHECK_OK(cfg.add_env("MYLIBCFG_A"));
    CHECK_OK(cfg.add_argv(2, argv));
    set_env("MYLIBCFG_A__PORT", nullptr);

    CHECK_EQ(cfg.value<int>("port", -1), 2222);
    CHECK_EQ(cfg.layers().size(), std::size_t{2});
    CHECK(cfg.layers()[1].kind == my::json::ConfigLayer::Kind::Argv);
}

MYTEST_CASE(config_argv_bad_key_reports_error) {
    const char* argv[] = {"prog", "--a..b=1", nullptr};
    my::json::Document doc;
    auto               st = my::json::document_from_argv(2, argv, doc);
    CHECK(!st);  // "a..b" 是非法路径，必须报错而不是静默忽略
}

MYTEST_CASE(config_merged_can_be_dumped) {
    my::json::Document defaults;
    CHECK_OK(defaults.set("a.b", 1));

    my::json::Document overlay;
    CHECK_OK(overlay.set("a.c", 2));

    my::json::Layered cfg;
    CHECK_OK(cfg.add_defaults(defaults));
    CHECK_OK(cfg.add_layer(overlay, my::json::ConfigLayer::Kind::Manual, "test"));

    const std::string text = cfg.merged().dump();
    CHECK(text.find("\"b\":1") != std::string::npos);
    CHECK(text.find("\"c\":2") != std::string::npos);
}
