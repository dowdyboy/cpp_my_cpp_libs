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

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "my_cpp_libs/core/status.hpp"
#include "my_cpp_libs/export.hpp"

namespace my::json {

using Json = nlohmann::json;

enum class Type { Null, Boolean, Integer, Floating, String, Array, Object, Unknown };

MYLIB_API const char* type_name(Type type) noexcept;

/// JSON 嵌套深度上限（parse 的词法扫描与 merge_into 的递归兜底共用）。
///
/// nlohmann 的解析器是递归下降，`[`×15000 这种 30 KB 的输入会让进程以
/// 0xC00000FD（栈溢出）直接死掉，Result/异常都接不住。256 层足够覆盖真实配置，
/// 再深基本可以判定是攻击或数据损坏。
inline constexpr std::size_t kMaxNestingDepth = 256;

// =============================================================================
//  路径解析（header-only，便于内联与单元测试）
// =============================================================================
namespace detail {

/// 路径里数组下标的上限（只作用于**写**路径）。
///
/// 存在的理由：`set("a[N]", v)` 会把 0..N-1 全部补成 null。N 由外部配置文件/网络报文
/// 决定时，一个 `a[1000000000]` 就能让进程 OOM —— 属于"输入放大"型问题。
/// 65536 足够覆盖真实配置里的数组，超过基本可以判定是攻击或写错了。
inline constexpr std::size_t kMaxSparseIndex = 65536;

/// 路径下标的可表示上限（解析期防 size_t 回绕）。单纯读取 `a[5000000]` 是合法的，
/// 所以读路径不套 kMaxSparseIndex，只挡回绕。
inline constexpr std::size_t kMaxPathIndex = (std::numeric_limits<std::size_t>::max)() / 16;

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
            // 合法形态只有两种：
            //   "a.b"            —— 前面有待落定的键
            //   "a[0].b" / "a[0][1].b" —— 前面刚解析完一个 [N]，此时没有 pending 键
            // 后者以前会被判成"空键"，于是 `a.b[0].c` 这种文档里写着的语法实际用不了。
            const bool after_index =
                !has_pending && pending.empty() && !tokens.empty() &&
                tokens.back().kind == PathToken::Kind::Index;
            if (!flush_key() && !after_index) {
                return Status(Errc::InvalidArgument, "JSON 路径中出现了空键: " + std::string(path));
            }
            ++i;
            if (i == path.size()) {
                return Status(Errc::InvalidArgument, "JSON 路径以 '.' 结尾: " + std::string(path));
            }
            // "." 后面必须还是一个键，不能紧跟 '[' 或 '.'
            if (path[i] == '[' || path[i] == '.') {
                return Status(Errc::InvalidArgument, "JSON 路径中出现了空键: " + std::string(path));
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
                // 只防 size_t 回绕（解析期不该拒绝大下标：读路径 a[500000] 是合法的，
                // 真正的内存放大风险在"写路径要给中间元素补 null"，那个上限在 resolve_create）
                if (idx > kMaxPathIndex) {
                    return Status(Errc::OutOfRange,
                                  "JSON 路径数组下标超出可表示范围: " + std::string(path));
                }
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
            // 稀疏下标必须设上限：set("a[1000000000]", 1) 会把中间元素全补成 null，
            // 一次调用就能吃掉几 GB 内存（配置文件是外部输入，等于一个放大攻击点）。
            if (tok.index > kMaxSparseIndex) {
                return Status(Errc::OutOfRange,
                              "JSON 路径数组下标过大（上限 " + std::to_string(kMaxSparseIndex) +
                                  "）: " + std::to_string(tok.index));
            }
            // nlohmann 的数组没有 resize()，缺位补 null
            while (cur->size() <= tok.index) cur->push_back(Json());
            cur = &(*cur)[tok.index];
        }
    }
    return cur;
}

/// 词法扫描：只数 `[` / `{` 的嵌套层数；字符串内的括号和转义序列不参与计数。
/// 在进入 nlohmann 之前先做这一遍，保证递归下降解析器不会因为超深输入爆栈。
/// 失败时还没有任何副作用（此时尚未构造 Document）。
inline Status check_nesting_depth(std::string_view text) {
    std::size_t depth     = 0;
    bool        in_string = false;
    bool        escaped   = false;

    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (escaped) {
                escaped = false;  // 被转义的字符即使 '"' 或 '\\' 也不结束字符串
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '[' || c == '{') {
            if (++depth > kMaxNestingDepth) {
                return Status(Errc::ParseError,
                              "JSON 嵌套深度超过上限 " + std::to_string(kMaxNestingDepth) +
                                  "（第 " + std::to_string(i) +
                                  " 字节），拒绝解析以避免栈溢出");
            }
        } else if ((c == ']' || c == '}') && depth > 0) {
            --depth;
        }
    }
    return Ok();
}

// ---------------------------------------------------------------- get<T> 数值范围检查
// nlohmann 的 get<T>() 对数值直接 static_cast，越界属于未定义/静默回绕：
//   get<unsigned short>(70000) -> 4464、get<int>(2^32) -> 0、get<int>(1e300) -> UB。
// 这里按目标类型做显式检查，失败返回 ParseError（不抛异常、不改文档）。

/// 目标整型的可接受范围（错误消息用）
template <class T>
inline std::string integral_range_text() {
    if constexpr (std::is_signed_v<T>) {
        return "[" +
               std::to_string(static_cast<long long>((std::numeric_limits<T>::min)())) + ", " +
               std::to_string(static_cast<long long>((std::numeric_limits<T>::max)())) + "]";
    } else {
        return "[0, " +
               std::to_string(static_cast<unsigned long long>((std::numeric_limits<T>::max)())) +
               "]";
    }
}

template <class T>
inline Status integral_error(std::string_view path, const std::string& detail) {
    return Status(Errc::ParseError,
                  "JSON 整型转换失败 [" + std::string(path) + "]: " + detail + "；目标为 " +
                      std::to_string(sizeof(T) * 8) + " 位" +
                      (std::is_signed_v<T> ? "有符号" : "无符号") + "整型，可接受范围 " +
                      integral_range_text<T>());
}

template <class T>
inline Result<T> get_integral_checked(const Json& v, std::string_view path) {
    switch (v.type()) {
        case Json::value_t::number_integer: {
            const std::int64_t raw = v.get_ref<const Json::number_integer_t&>();
            if constexpr (std::is_signed_v<T>) {
                const std::int64_t lo =
                    static_cast<std::int64_t>((std::numeric_limits<T>::min)());
                const std::int64_t hi =
                    static_cast<std::int64_t>((std::numeric_limits<T>::max)());
                if (raw < lo || raw > hi) {
                    return integral_error<T>(
                        path, "实际 integer=" + v.dump() + "，超出范围");
                }
            } else {
                if (raw < 0) {
                    return integral_error<T>(
                        path, "实际 integer=" + v.dump() + "，不能转换为无符号整型");
                }
                const std::uint64_t hi =
                    static_cast<std::uint64_t>((std::numeric_limits<T>::max)());
                if (static_cast<std::uint64_t>(raw) > hi) {
                    return integral_error<T>(
                        path, "实际 integer=" + v.dump() + "，超出范围");
                }
            }
            return static_cast<T>(raw);
        }
        case Json::value_t::number_unsigned: {
            const std::uint64_t raw = v.get_ref<const Json::number_unsigned_t&>();
            const std::uint64_t hi =
                static_cast<std::uint64_t>((std::numeric_limits<T>::max)());
            if (raw > hi) {
                return integral_error<T>(
                    path, "实际 unsigned=" + v.dump() + "，超出范围");
            }
            return static_cast<T>(raw);
        }
        case Json::value_t::number_float: {
            const double raw = v.get_ref<const Json::number_float_t&>();
            if (!std::isfinite(raw)) {
                return integral_error<T>(
                    path, "实际 floating 值是 NaN/Inf，不是有限整数值");
            }
            if (std::trunc(raw) != raw) {
                return integral_error<T>(
                    path, "实际 floating=" + v.dump() + "，不是整数值");
            }
            // 浮点边界用 2 的幂判断：int64/uint64 的 max 无法在 double 里精确表示，
            // 直接和 INT64_MAX 比较会把 2^63 当成"不超界"，转换仍是 UB。
            if constexpr (std::is_signed_v<T>) {
                const double lo = -std::ldexp(1.0, std::numeric_limits<T>::digits);
                const double hi = std::ldexp(1.0, std::numeric_limits<T>::digits);
                if (!(raw >= lo && raw < hi)) {
                    return integral_error<T>(
                        path, "实际 floating=" + v.dump() + "，超出范围");
                }
            } else {
                const double hi = std::ldexp(1.0, std::numeric_limits<T>::digits);
                if (!(raw >= 0.0 && raw < hi)) {
                    return integral_error<T>(
                        path, "实际 floating=" + v.dump() + "，超出范围");
                }
            }
            return static_cast<T>(raw);
        }
        default:
            return integral_error<T>(
                path, "实际类型 " + std::string(v.type_name()) +
                          "，只接受 number_integer/number_unsigned/整值 number_float");
    }
}

/// 浮点目标：整数与浮点都接受，但拒绝不能表示为有限 double 的值
template <class T>
inline Result<T> get_floating_checked(const Json& v, std::string_view path) {
    double raw = 0.0;
    switch (v.type()) {
        case Json::value_t::number_integer:
            raw = static_cast<double>(v.get_ref<const Json::number_integer_t&>());
            break;
        case Json::value_t::number_unsigned:
            raw = static_cast<double>(v.get_ref<const Json::number_unsigned_t&>());
            break;
        case Json::value_t::number_float:
            raw = v.get_ref<const Json::number_float_t&>();
            break;
        default:
            return Status(Errc::ParseError,
                          "JSON 浮点转换失败 [" + std::string(path) + "]: 实际类型 " +
                              v.type_name() + "，期望 number");
    }
    if (!std::isfinite(raw)) {
        return Status(Errc::ParseError,
                      "JSON 浮点转换失败 [" + std::string(path) +
                          "]: 值不能表示为有限 double");
    }
    return static_cast<T>(raw);
}

/// get<T> 的统一入口：整型/浮点自检，其余（bool/string/容器/自定义结构体）
/// 保持 nlohmann 原语义，异常由 Document::get 的 catch 兜底。
template <class T>
inline Result<T> get_checked(const Json& v, std::string_view path) {
    if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
        return get_integral_checked<T>(v, path);
    } else if constexpr (std::is_floating_point_v<T>) {
        return get_floating_checked<T>(v, path);
    } else {
        return v.get<T>();
    }
}

}  // namespace detail

// =============================================================================
//  Document
// =============================================================================
class Document {
public:
    /// 默认构造为空对象 `{}`
    Document() : value_(Json::object()) {}
    /// 逃生舱：接管任意 Json。**不受 kMaxNestingDepth 保护**，外部构造的超深
    /// Json 在 dump()/merge_into() 时仍需调用方自行保证深度。
    explicit Document(Json value) : value_(std::move(value)) {}

    Document(const Document&)                = default;
    Document& operator=(const Document&)     = default;
    Document(Document&&) noexcept            = default;
    Document& operator=(Document&&) noexcept = default;

    // ---------------------------------------------------------------- 序列化
    static Result<Document> parse(std::string_view text) {
        // 先做纯词法扫描：nlohmann 是递归下降解析器，15000 层 `[` 会让进程
        // 0xC00000FD 爆栈（不是异常，Result 接不住）。超深输入在这里就拒绝，
        // 此时还没有任何副作用。
        if (auto st = detail::check_nesting_depth(text); !st) return st;
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

    /// 序列化。非法 UTF-8 会被替换而不是抛异常。
    /// 注意：dump 是递归实现，只对 parse 出来（深度 ≤ kMaxNestingDepth）或
    /// 调用方自行保证深度的文档安全，逃生舱构造的超深 Json 不在此列。
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
            // 整型/浮点在这里做范围与整值检查，避免 nlohmann 直接 static_cast
            // 造成静默截断/回绕（错误消息带路径、实际值与目标范围）。
            return detail::get_checked<T>(*p, path);
        } catch (const std::exception& e) {
            return Status(Errc::ParseError,
                          "JSON 类型转换失败 [" + std::string(path) + "]: " + e.what());
        } catch (...) {
            return Status(Errc::ParseError, "JSON 类型转换失败 [" + std::string(path) + "]");
        }
    }

    /// 便捷读取：路径不存在**或类型不匹配**都返回 fallback。
    /// 配置里需要区分"缺失/类型写错"时请用 get<T>() 检查 Status。
    template <class T>
    T get_or(std::string_view path, T fallback) const {
        auto r = get<T>(path);
        return r ? r.value() : std::move(fallback);
    }

    template <class T>
    Status set(std::string_view path, T&& value) {
        auto tokens = detail::parse_path(path);
        if (!tokens) return tokens.status();

        // 先在副本上解析/创建，成功后才替换 value_{}：失败不能留下半截中间节点
        // （例如 set("a.b[1000000000]",1) 不应把文档改成 {"a":{"b":[]}}）。
        try {
            Json copy   = value_;
            auto target = detail::resolve_create(copy, tokens.value());
            if (!target) return target.status();

            *target.value() = Json(std::forward<T>(value));
            value_          = std::move(copy);
            return Ok();
        } catch (const std::exception& e) {
            return Status(Errc::RuntimeError, std::string("JSON 赋值失败: ") + e.what());
        } catch (...) {
            return Status(Errc::RuntimeError, "JSON 赋值失败: 未知错误");
        }
    }

    Status erase(std::string_view path);

    /// 对象键列表；path 指向非对象时返回空表
    std::vector<std::string> keys(std::string_view path = {}) const;

    // ---------------------------------------------------------------- 逃生舱
    /// 直接拿到底层 Json。通过这里塞进来的节点**绕过** parse 的嵌套深度上限，
    /// 超深结构在 dump()/merge_into() 的递归路径上需要调用方自行保证深度。
    Json&       raw() noexcept { return value_; }
    const Json& raw() const noexcept { return value_; }

private:
    Json value_;
};

}  // namespace my::json
