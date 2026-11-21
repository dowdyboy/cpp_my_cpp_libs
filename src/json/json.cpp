#include "my_cpp_libs/json/json.hpp"
#include "my_cpp_libs/core/path_utf8.hpp"

#include "my_cpp_libs/json/config.hpp"  // merge_into / document_from_env / Layered

#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

// 环境变量表：三个平台的取法都不一样，这里统一声明一次。
//   POSIX  : extern char** environ（<unistd.h> 里已有声明）
//   MinGW  : _environ
//   MSVC   : __p__environ() 返回 char***
#if defined(_WIN32)
extern "C" char** _environ;
#  if defined(_MSC_VER)
extern "C" char*** __p__environ(void);
#  endif
#else
#  include <unistd.h>
#endif

namespace my::json {

const char* type_name(Type type) noexcept {
    switch (type) {
        case Type::Null: return "null";
        case Type::Boolean: return "boolean";
        case Type::Integer: return "integer";
        case Type::Floating: return "floating";
        case Type::String: return "string";
        case Type::Array: return "array";
        case Type::Object: return "object";
        case Type::Unknown: default: return "unknown";
    }
}

Result<Document> Document::parse_file(const std::string& path) {
    std::ifstream in(core::to_path(path), std::ios::binary);
    if (!in) {
        return Status(Errc::IoError, "无法打开 JSON 文件: " + path);
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return Status(Errc::IoError, "读取 JSON 文件失败: " + path);
    }
    auto doc = parse(text);
    if (!doc) {
        return Status(doc.status().code(),
                      "文件 " + path + " 解析失败: " + doc.status().message());
    }
    return doc;
}

Status Document::save_file(const std::string& path, int indent) const {
    std::ofstream out(core::to_path(path), std::ios::binary | std::ios::trunc);
    if (!out) {
        return Status(Errc::IoError, "无法写入 JSON 文件: " + path);
    }
    const std::string text = dump(indent);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (indent >= 0) out.put('\n');
    out.flush();
    if (!out) {
        return Status(Errc::IoError, "写入 JSON 文件失败: " + path);
    }
    return Ok();
}

namespace {

/// 只读导航（不创建节点），失败返回 nullptr
Json* navigate(Json& root, const std::vector<detail::PathToken>& tokens) noexcept {
    Json* cur = &root;
    for (const auto& tok : tokens) {
        if (tok.kind == detail::PathToken::Kind::Key) {
            if (!cur->is_object()) return nullptr;
            auto it = cur->find(tok.key);
            if (it == cur->end()) return nullptr;
            cur = &(*it);
        } else {
            if (!cur->is_array() || tok.index >= cur->size()) return nullptr;
            cur = &(*cur)[tok.index];
        }
    }
    return cur;
}

}  // namespace

Status Document::erase(std::string_view path) {
    auto tokens = detail::parse_path(path);
    if (!tokens) return tokens.status();

    auto& toks = tokens.value();
    if (toks.empty()) {
        value_ = Json::object();
        return Ok();
    }

    const detail::PathToken last = toks.back();
    toks.pop_back();

    Json* parent = navigate(value_, toks);
    if (parent == nullptr) {
        return Status(Errc::NotFound, "JSON 路径不存在: " + std::string(path));
    }

    if (last.kind == detail::PathToken::Kind::Key) {
        if (!parent->is_object()) {
            return Status(Errc::InvalidArgument, "JSON 父节点不是对象: " + std::string(path));
        }
        if (parent->erase(last.key) == 0) {
            return Status(Errc::NotFound, "JSON 键不存在: " + std::string(path));
        }
        return Ok();
    }

    if (!parent->is_array() || last.index >= parent->size()) {
        return Status(Errc::NotFound, "JSON 数组索引越界: " + std::string(path));
    }
    parent->erase(parent->begin() + static_cast<std::ptrdiff_t>(last.index));
    return Ok();
}

std::vector<std::string> Document::keys(std::string_view path) const {
    std::vector<std::string> out;
    auto tokens = detail::parse_path(path);
    if (!tokens) return out;
    const Json* p = detail::resolve_const(value_, tokens.value());
    if (p == nullptr || !p->is_object()) return out;

    out.reserve(p->size());
    for (auto it = p->begin(); it != p->end(); ++it) {
        out.push_back(it.key());
    }
    return out;
}

// =============================================================================
//  分层配置（json/config.hpp）
// =============================================================================
namespace {

/// 深合并：对象递归，数组/标量整体替换。
///
/// depth 是兜底用的递归深度：Document::parse 保证内部文档 ≤ kMaxNestingDepth，
/// 但 Document(Json) / raw() 是逃生舱，外部可以塞进任意深的 Json。超过上限返回
/// RuntimeError，保证 **对象递归合并** 这条路径不会把调用栈打穿（此时 base 可能
/// 已被部分修改；只有外部构造的超深输入才会走到这里）。注意：整体赋值
/// `base[key] = overlay[key]` 仍走 nlohmann 自己的递归拷贝，逃生舱子树太深时
/// 需要调用方自行保证深度。
Status merge_json(Json& base, const Json& overlay, std::size_t depth) {
    if (depth > kMaxNestingDepth) {
        return Status(Errc::RuntimeError,
                      "JSON 深合并深度超过上限 " + std::to_string(kMaxNestingDepth) +
                          "（外部构造的 Json 需自行保证深度）");
    }
    if (base.is_object() && overlay.is_object()) {
        for (auto it = overlay.begin(); it != overlay.end(); ++it) {
            auto at = base.find(it.key());
            if (at != base.end()) {
                if (auto st = merge_json(*at, it.value(), depth + 1); !st) return st;
            } else {
                base[it.key()] = it.value();
            }
        }
        return Ok();
    }
    base = overlay;
    return Ok();
}

/// 尽量按 JSON 解析值；失败就当字符串（配置文件里写 8080 / true / "abc" 都自然）
Json parse_scalar_or_string(const std::string& text) {
    auto doc = Document::parse(text);
    if (doc) return doc.value().raw();
    return Json(text);
}

/// 环境变量表。三个平台取法不同，都拿失败也行（返回空表即可）。
char** environ_table() noexcept {
#if defined(_MSC_VER)
    return *__p__environ();
#elif defined(_WIN32)
    return _environ;
#else
    return environ;
#endif
}

}  // namespace

Status merge_into(Document& base, const Document& overlay) {
    try {
        return merge_json(base.raw(), overlay.raw(), 0);
    } catch (const std::exception& e) {
        return Status(Errc::RuntimeError, std::string("配置合并失败: ") + e.what());
    }
}

Status document_from_env(const std::string& prefix, Document& out) {
    if (prefix.empty()) {
        return Status(Errc::InvalidArgument, "环境变量前缀为空");
    }
    const std::string head = prefix + "__";

    Document layer;
    char**   env = environ_table();
    if (env == nullptr) {
        out = std::move(layer);
        return Ok();
    }

    for (char** cur = env; *cur != nullptr; ++cur) {
        const std::string entry(*cur);
        const auto        eq = entry.find('=');
        if (eq == std::string::npos) continue;
        if (entry.size() < head.size() || entry.compare(0, head.size(), head) != 0) continue;

        const std::string raw_key = entry.substr(head.size(), eq - head.size());
        if (raw_key.empty()) continue;
        const std::string value_text = entry.substr(eq + 1);

        // SERVER__PORT -> server.port
        std::string path;
        path.reserve(raw_key.size());
        for (std::size_t i = 0; i < raw_key.size(); ++i) {
            if (raw_key[i] == '_' && i + 1 < raw_key.size() && raw_key[i + 1] == '_') {
                path.push_back('.');
                ++i;
                continue;
            }
            char c = raw_key[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            path.push_back(c);
        }
        if (path.empty()) continue;

        if (auto st = layer.set(path, parse_scalar_or_string(value_text)); !st) {
            return Status(st.code(), "环境变量 " + entry.substr(0, eq) + " 无法映射: " + st.message());
        }
    }

    out = std::move(layer);
    return Ok();
}

Status document_from_argv(int argc, const char* const* argv, Document& out, int start_index) {
    if (argv == nullptr || argc <= start_index) {
        out = Document();
        return Ok();
    }

    Document layer;
    for (int i = start_index; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg.size() < 3 || arg[0] != '-' || arg[1] != '-') continue;  // 不是长选项

        std::string_view rest = arg.substr(2);
        std::string_view key;
        std::string_view value;
        const auto       eq = rest.find('=');
        bool             has_value = false;
        if (eq != std::string_view::npos) {
            key       = rest.substr(0, eq);
            value     = rest.substr(eq + 1);
            has_value = true;
        } else {
            key = rest;
            // --key value：下一个参数不是选项时当作值
            if (i + 1 < argc) {
                const std::string_view next(argv[i + 1]);
                if (next.size() < 2 || next[0] != '-' || next[1] != '-') {
                    value     = next;
                    has_value = true;
                    ++i;
                }
            }
        }
        if (key.empty()) continue;

        const std::string key_str(key);
        if (auto st = layer.set(key_str, has_value ? parse_scalar_or_string(std::string(value))
                                                   : Json(true));
            !st) {
            return Status(st.code(), "命令行参数 --" + key_str + " 无法解析: " + st.message());
        }
    }

    out = std::move(layer);
    return Ok();
}

bool Layered::io_exists(const std::string& path) {
    std::ifstream in(core::to_path(path), std::ios::binary);
    return static_cast<bool>(in);
}

}  // namespace my::json

