#pragma once
// =============================================================================
//  my::Status / my::Result<T> —— 全库统一的错误处理原语
//
//  设计取舍：对外 API 一律不使用异常。
//    * 嵌入式（Zynq）上常以 -fno-exceptions 交付，异常会破坏可移植性
//    * 异常让"错误必须被处理"这件事在签名上看不见
//  三方库（nlohmann / ORT）的异常在本库边界处被翻译成 Status。
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "my_cpp_libs/export.hpp"

namespace my {

// -----------------------------------------------------------------------------
//  错误码：刻意保持"粗粒度 + 少量"，便于跨模块映射
// -----------------------------------------------------------------------------
enum class Errc : int {
    Ok = 0,

    // 参数/调用方错误
    InvalidArgument,
    OutOfRange,
    NotFound,
    AlreadyExists,

    // 资源/环境错误
    IoError,
    ParseError,
    Unsupported,
    NotInitialized,
    Busy,

    // 运行时错误
    RuntimeError,
    Cancelled,
    Timeout,

    Unknown
};

/// 错误码的可读名称（稳定字符串，可安全用于日志）
MYLIB_API const char* errc_name(Errc code) noexcept;

// -----------------------------------------------------------------------------
//  Status
// -----------------------------------------------------------------------------
/// 失败必须被处理：显式丢弃要写 `(void)`，否则编译器告警。
/// （Result<T> 已经是 [[nodiscard]]，Status 之前漏了，导致"启动失败被吞掉"这类
///   问题只能靠读代码发现，见 src/http/http.cpp 里那批 `(void)route(...)`。）
class [[nodiscard]] Status {
public:
    /// 成功状态
    Status() noexcept = default;

    Status(Errc code, std::string message = {}) noexcept
        : code_(code), message_(std::move(message)) {}

    static Status success() noexcept { return Status{}; }

    [[nodiscard]] bool ok() const noexcept { return code_ == Errc::Ok; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] bool failed() const noexcept { return !ok(); }

    [[nodiscard]] Errc code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    /// "code: message" 形式的完整描述
    [[nodiscard]] MYLIB_API std::string to_string() const;

    /// 追加/替换上下文信息（调试用，返回自身以支持链式）
    Status& with_message(std::string message) noexcept {
        message_ = std::move(message);
        return *this;
    }

private:
    Errc        code_ = Errc::Ok;
    std::string message_;
};

inline Status Ok() noexcept { return Status{}; }

// -----------------------------------------------------------------------------
//  Result<T>
//
//  用法：
//      Result<int> r = parse_int("42");
//      if (!r) { log::error("{}", r.status().to_string()); return; }
//      int v = r.value();
//
//  注意：value() 不做检查，必须先用 ok() 判定；违反约定时直接 abort
//        （这是编程错误，不是运行期可恢复错误）。
// -----------------------------------------------------------------------------
template <class T>
class [[nodiscard]] Result {
public:
    static_assert(!std::is_reference_v<T>, "Result<T> 不支持引用类型");
    static_assert(!std::is_same_v<std::decay_t<T>, Status>, "Result<Status> 请直接用 Status");

    // --- 构造 ---
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(Status status) : storage_(std::in_place_index<1>, std::move(status)) {}

    // 允许从可隐式转换的类型构造，例如 Result<double> r = 1.0f;
    template <class U, class = std::enable_if_t<
                           std::is_constructible_v<T, U&&> && !std::is_same_v<std::decay_t<U>, T> &&
                           !std::is_same_v<std::decay_t<U>, Status>>>
    Result(U&& value) : storage_(std::in_place_index<0>, T(std::forward<U>(value))) {}

    // --- 观测 ---
    [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const Status& status() const noexcept {
        return ok() ? ok_status() : std::get<1>(storage_);
    }

    [[nodiscard]] T& value() noexcept { return *checked(); }
    [[nodiscard]] const T& value() const noexcept { return *checked(); }

    [[nodiscard]] T& operator*() noexcept { return value(); }
    [[nodiscard]] const T& operator*() const noexcept { return value(); }
    [[nodiscard]] T* operator->() noexcept { return &value(); }
    [[nodiscard]] const T* operator->() const noexcept { return &value(); }

    /// 失败时返回 fallback，成功时返回持有值
    [[nodiscard]] T value_or(T fallback) const {
        return ok() ? std::get<0>(storage_) : std::move(fallback);
    }

    // --- 变换 ---
    template <class F>
    auto map(F&& f) const -> Result<std::decay_t<std::invoke_result_t<F, const T&>>> {
        using U = std::decay_t<std::invoke_result_t<F, const T&>>;
        if (!ok()) return Result<U>{status()};
        return Result<U>{std::invoke(std::forward<F>(f), std::get<0>(storage_))};
    }

    template <class F>
    auto map(F&& f) -> Result<std::decay_t<std::invoke_result_t<F, T&>>> {
        using U = std::decay_t<std::invoke_result_t<F, T&>>;
        if (!ok()) return Result<U>{status()};
        return Result<U>{std::invoke(std::forward<F>(f), std::get<0>(storage_))};
    }

private:
    static const Status& ok_status() noexcept {
        static const Status s{};
        return s;
    }

    T* checked() noexcept {
        T* p = std::get_if<0>(&storage_);
        if (p == nullptr) {
            // 违反前置条件就是编程错误，该炸；但炸之前留一行线索，
            // 否则嵌入式上表现成"莫名重启"，什么也查不到。
            std::fprintf(stderr, "[my_cpp_libs] Result<T>::value() 在失败状态被调用: %s\n",
                         std::get<1>(storage_).to_string().c_str());
            std::abort();
        }
        return p;
    }
    const T* checked() const noexcept {
        const T* p = std::get_if<0>(&storage_);
        if (p == nullptr) {
            std::fprintf(stderr, "[my_cpp_libs] Result<T>::value() 在失败状态被调用: %s\n",
                         std::get<1>(storage_).to_string().c_str());
            std::abort();
        }
        return p;
    }

    std::variant<T, Status> storage_;
};

}  // namespace my
