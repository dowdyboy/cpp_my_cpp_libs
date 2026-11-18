#pragma once
// =============================================================================
//  my::json 的分层配置（不新建组件，直接长在 json 上）
//
//  为什么不做成独立组件：配置的本质就是"JSON 的深合并"，独立组件只会多一层
//  类型转换。见 docs/01 §5 的建议。
//
//  典型用法：
//      my::json::Layered cfg;
//      cfg.add_defaults(defaults);                    // 代码里的默认值
//      cfg.add_file_if_exists("config.json");         // 文件
//      cfg.add_env("MYAPP");                           // MYAPP__SERVER__PORT=8080
//      cfg.add_argv(argc, argv);                       // --server.port=9090
//      int port = cfg.value<int>("server.port", 8080);
//
//  优先级：defaults < file < env < argv（后加的覆盖先加的）。
//  合并规则：对象递归深合并，数组与标量**整体覆盖**
//  （数组做逐元素合并会让"删掉一项"变成不可能，工程上更糟）。
//
//  注意：落盘请用 io::write_file(path, cfg.merged().dump(2)) 拿到原子写，
//  Document::save_file 是直接 truncate 写的（历史行为，未改）。
// =============================================================================

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "my_cpp_libs/json/json.hpp"

namespace my::json {

/// 深合并：overlay 覆盖 base。对象递归，数组/标量整体替换。
MYLIB_API Status merge_into(Document& base, const Document& overlay);

/// 环境变量 → Document。
/// 约定：前缀 + "__" 之后开始，层级用 "__" 分隔，键名转小写。
///   例：MYAPP__SERVER__PORT=8080  →  {"server":{"port":8080}}
/// 值按 JSON 解析；解析失败则当字符串（所以 "abc" 和 "\"abc\"" 等价）。
MYLIB_API Status document_from_env(const std::string& prefix, Document& out);

/// 命令行 → Document。只认 "--" 开头的参数：
///   --server.port=8080     →  {"server":{"port":8080}}
///   --server.port 8080     →  同上（下一个参数不以 "--" 开头时当作值）
///   --verbose              →  {"verbose":true}
MYLIB_API Status document_from_argv(int argc, const char* const* argv, Document& out,
                                    int start_index = 1);

/// 一层配置的来源记录（排查"这个值到底从哪来的"用）
struct ConfigLayer {
    enum class Kind { Defaults, File, Env, Argv, Manual };
    Kind        kind = Kind::Manual;
    std::string name;
};

class Layered {
public:
    Layered() = default;

    Layered& add_defaults(Document defaults) {
        (void)add_layer(std::move(defaults), ConfigLayer::Kind::Defaults, "defaults");
        return *this;
    }

    /// 读文件并合并；解析/读取失败返回错误（调用方决定要不要继续）
    Status add_file(const std::string& path) {
        auto doc = Document::parse_file(path);
        if (!doc) return doc.status();
        return add_layer(std::move(doc.value()), ConfigLayer::Kind::File, path);
    }

    /// 文件不存在就跳过（存在但解析失败仍然报错 —— 那是配置写错了，不该静默）
    Status add_file_if_exists(const std::string& path) {
        if (!io_exists(path)) return Ok();
        return add_file(path);
    }

    Status add_env(const std::string& prefix) {
        Document doc;
        if (auto st = document_from_env(prefix, doc); !st) return st;
        return add_layer(std::move(doc), ConfigLayer::Kind::Env, "env:" + prefix);
    }

    Status add_argv(int argc, const char* const* argv, int start_index = 1) {
        Document doc;
        if (auto st = document_from_argv(argc, argv, doc, start_index); !st) return st;
        return add_layer(std::move(doc), ConfigLayer::Kind::Argv, "argv");
    }

    Status add_layer(Document doc, ConfigLayer::Kind kind, std::string name) {
        if (auto st = merge_into(merged_, doc); !st) return st;
        layers_.push_back(ConfigLayer{kind, std::move(name)});
        return Ok();
    }

    const Document&                 merged() const noexcept { return merged_; }
    const std::vector<ConfigLayer>& layers() const noexcept { return layers_; }

    template <class T>
    T value(std::string_view path, T fallback) const {
        return merged_.get_or<T>(path, std::move(fallback));
    }
    bool has(std::string_view path) const { return merged_.contains(path); }

private:
    /// 只判"文件存在"，不引 io 组件（保持 json 不依赖 io）
    static bool io_exists(const std::string& path);

    Document                 merged_;
    std::vector<ConfigLayer> layers_;
};

}  // namespace my::json
