#include "my_cpp_libs/json/json.hpp"

#include <fstream>
#include <iterator>
#include <string>

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
    std::ifstream in(path, std::ios::binary);
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
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
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

}  // namespace my::json
