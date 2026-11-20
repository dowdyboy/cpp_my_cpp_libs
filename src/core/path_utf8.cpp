// =============================================================================
//  UTF-8 <-> std::filesystem::path 的实现（Windows 上显式走 UTF-16）
//
//  把 windows.h 关在这个 TU 里，公共头只暴露声明 —— 否则每个包含
//  core/path_utf8.hpp 的源文件都会被拖进 windows.h 的宏污染。
// =============================================================================

#include "my_cpp_libs/core/path_utf8.hpp"

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

namespace my::core {

std::filesystem::path to_path(std::string_view utf8) {
#if defined(_WIN32)
    if (utf8.empty()) return std::filesystem::path();
    const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (wlen <= 0) {
        // 不是合法 UTF-8：退回原样（至少不崩，行为与 0.1.7 之前一致）
        return std::filesystem::path(std::string(utf8));
    }
    std::wstring w(static_cast<std::size_t>(wlen), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), wlen);
    return std::filesystem::path(w);
#else
    return std::filesystem::path(std::string(utf8));
#endif
}

std::string from_path(const std::filesystem::path& p) {
#if defined(_WIN32)
    const std::wstring w = p.native();
    if (w.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr,
                                        0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                          nullptr);
    return s;
#else
    return p.string();
#endif
}

}  // namespace my::core
