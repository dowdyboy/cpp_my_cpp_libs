#pragma once
// =============================================================================
//  my::json —— 统一 JSON 接口（底层 nlohmann/json）
//
//  相比裸用 nlohmann 的收益
//    1. 路径式访问："sensor.imu[0].name"
//    2. 全程 Result<T>/Status，不抛异常
//    3. 文件读写、类型查询、增删改的统一形态
//
//  逃生舱：Document::raw() 直接返回 nlohmann::json&，需要高级特性时可无缝下探。
// =============================================================================

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "my_cpp_libs/core/status.hpp"
#include "my_cpp_libs/export.hpp"

namespace my::json {

using Json = nlohmann::json;

enum class Type { Null, Boolean, Integer, Floating, String, Array, Object, Unknown };

MYLIB_API const char* type_name(Type type) noexcept;

// =============================================================================
//  路径解析（header-only，便于内联与单元测试）
// =============================================================================
namespace detail {

struct PathToken {
    enum class Kind { Key, Index };
    Kind        kind  = Kind::Key;
    std::string key;
    std::size_t index = 0;
};

/// 支持语法：`a.b[2].c`；空串表示根节点；数字索引只支持 [N]
inline Result<std::vector<PathToken>> parse_path(std::string_view path) {
    std::vector<PathToken> tokens;
    if (path.empty()) return tokens;

    std::size_t i = 0;
    std::string pending;
    bool        has_pending = false;

    auto flush_key = [&]() {
        if (has_pending || !pending.empty()) {
            PathToken t;
            t.kind = PathToken::Kind::Key;
            t.key  = pending;
            tokens.push_back(std::move(t));
            pending.clear();
            has_pending = false;
            return true;
        }
        return false;
    };

    while (i < path.size()) {
        const char c = path[i];
        if (c == '.') {
            if (!flush_key()) {
                return Status(Errc::InvalidArgument, "JSON 路径中出现了空键: " + std::string(path));
            }
            ++i;
            if (i == path.size()) {
                return Status(Errc::InvalidArgument, "JSON 路径以 '.' 结尾: " + std::string(path));
            }
            continue;
        }
        if (c == '[') {
            flush_key();
            const std::size_t close = path.find(']', i);
            if (close == std::string_view::npos) {
                return Status(Errc::InvalidArgument, "JSON 路径缺少 ']': " + std::string(path));
            }
            const std::string_view num = path.substr(i + 1, close - i - 1);
            if (num.empty()) {
                return Status(Errc::InvalidArgument, "JSON 路径中索引为空: " + std::string(path));
            }
            std::size_t idx = 0;
            for (char d : num) {
                if (d < '0' || d > '9') {
                    return Status(Errc::InvalidArgument,
                                  "JSON 路径索引必须是非负整数: " + std::string(path));
                }
                idx = idx * 10 + static_cast<std::size_t>(d - '0');
            }
            PathToken t;
            t.kind  = PathToken::Kind::Index;
            t.index = idx;
            tokens.push_back(std::move(t));
            i = close + 1;
            continue;
        }
        pending.push_back(c);
        has_pending = true;
        ++i;
    }
    flush_key();
    return tokens;
}

/// 只读解析：不存在返回 nullptr（算法与 my::json::Document::get 共用）
inline const Json* resolve_const(const Json& root, const std::vector<PathToken>& tokens) noexcept {
    const Json* cur = &root;
    for (const auto& tok : tokens) {
        if (tok.kind == PathToken::Kind::Key) {
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

/// 可写解析：缺失的中间节点按需创建（对象或数组）
inline Result<Json*> resolve_create(Json& root, const std::vector<PathToken>& tokens) {
    Json* cur = &root;
    for (const auto& tok : tokens) {
        if (tok.kind == PathToken::Kind::Key) {
            if (cur->is_null()) *cur = Json::object();
            if (!cur->is_object()) {
                return Status(Errc::InvalidArgument, "JSON 路径上的中间节点不是对象: " + tok.key);
            }
            cur = &(*cur)[tok.key];
        } else {
            if (cur->is_null()) *cur = Json::array();
            if (!cur->is_array()) {
                return Status(Errc::InvalidArgument, "JSON 路径上的中间节点不是数组");
            }
            // nlohmann 的数组没有 resize()，缺位补 null
            while (cur->size() <= tok.index) cur->push_back(Json());
            cur = &(*cur)[tok.index];
        }
    }
    return cur;
}

}  // namespace detail

// =============================================================================
//  Document
// =============================================================================
class Document {
public:
    /// 默认构造为空对象 `{}`
    Document() : value_(Json::object()) {}
    explicit Document(Json value) : value_(std::move(value)) {}

    Document(const Document&)                = default;
    Document& operator=(const Document&)     = default;
    Document(Document&&) noexcept            = default;
    Document& operator=(Document&&) noexcept = default;

    // ---------------------------------------------------------------- 序列化
    static Result<Document> parse(std::string_view text) {
        try {
            return Document(Json::parse(text.begin(), text.end()));
        } catch (const std::exception& e) {
            return Status(Errc::ParseError, std::string("JSON 解析失败: ") + e.what());
        } catch (...) {
            return Status(Errc::ParseError, "JSON 解析失败: 未知错误");
        }
    }

    static Result<Document> parse_file(const std::string& path);

    /// 写文件（indent<0 表示紧凑输出）
    Status save_file(const std::string& path, int indent = 2) const;

    /// 序列化。非法 UTF-8 会被替换而不是抛异常
    std::string dump(int indent = -1) const {
        return value_.dump(indent, ' ', false, Json::error_handler_t::replace);
    }

    // ---------------------------------------------------------------- 类型
    static Type type_of_value(const Json& v) noexcept {
        switch (v.type()) {
            case Json::value_t::null: return Type::Null;
            case Json::value_t::boolean: return Type::Boolean;
            case Json::value_t::number_integer:
            case Json::value_t::number_unsigned: return Type::Integer;
            case Json::value_t::number_float: return Type::Floating;
            case Json::value_t::string: return Type::String;
            case Json::value_t::array: return Type::Array;
            case Json::value_t::object: return Type::Object;
            default: return Type::Unknown;
        }
    }

    Type type() const noexcept { return type_of_value(value_); }

    bool is_null() const noexcept { return value_.is_null(); }
    bool is_object() const noexcept { return value_.is_object(); }
    bool is_array() const noexcept { return value_.is_array(); }
    bool empty() const noexcept { return value_.empty(); }
    std::size_t size() const noexcept { return value_.size(); }

    /// 路径处容器的大小；不存在返回 0
    std::size_t size(std::string_view path) const {
        auto tokens = detail::parse_path(path);
        if (!tokens) return 0;
        const Json* p = detail::resolve_const(value_, tokens.value());
        return p != nullptr ? p->size() : 0;
    }

    // ---------------------------------------------------------------- 访问
    bool contains(std::string_view path) const {
        auto tokens = detail::parse_path(path);
        if (!tokens) return false;
        return detail::resolve_const(value_, tokens.value()) != nullptr;
    }

    /// 路径处值的类型；不存在返回 Type::Unknown
    Type type_of(std::string_view path) const {
        auto tokens = detail::parse_path(path);
        if (!tokens) return Type::Unknown;
        const Json* p = detail::resolve_const(value_, tokens.value());
        if (p == nullptr) return Type::Unknown;
        return type_of_value(*p);
    }

    template <class T>
    Result<T> get(std::string_view path) const {
        auto tokens = detail::parse_path(path);
        if (!tokens) return tokens.status();

        const Json* p = detail::resolve_const(value_, tokens.value());
        if (p == nullptr) {
            return Status(Errc::NotFound, "JSON 路径不存在: " + std::string(path));
        }
        try {
            return p->get<T>();
        } catch (const std::exception& e) {
            return Status(Errc::ParseError,
                          "JSON 类型转换失败 [" + std::string(path) + "]: " + e.what());
        } catch (...) {
            return Status(Errc::ParseError, "JSON 类型转换失败 [" + std::string(path) + "]");
        }
    }

    template <class T>
    T get_or(std::string_view path, T fallback) const {
        auto r = get<T>(path);
        return r ? r.value() : std::move(fallback);
    }

    template <class T>
    Status set(std::string_view path, T&& value) {
        auto tokens = detail::parse_path(path);
        if (!tokens) return tokens.status();

        auto target = detail::resolve_create(value_, tokens.value());
        if (!target) return target.status();

        try {
            *target.value() = Json(std::forward<T>(value));
            return Ok();
        } catch (const std::exception& e) {
            return Status(Errc::RuntimeError, std::string("JSON 赋值失败: ") + e.what());
        }
    }

    Status erase(std::string_view path);

    /// 对象键列表；path 指向非对象时返回空表
    std::vector<std::string> keys(std::string_view path = {}) const;

    // ---------------------------------------------------------------- 逃生舱
    Json&       raw() noexcept { return value_; }
    const Json& raw() const noexcept { return value_; }

private:
    Json value_;
};

}  // namespace my::json
