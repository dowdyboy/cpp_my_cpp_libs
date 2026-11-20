#pragma once
// =============================================================================
//  my::core::to_path / from_path —— UTF-8 std::string 与 std::filesystem::path 的互转
//
//  为什么需要它：Windows 上 `fs::path(std::string)` 与 `std::ifstream(std::string)`
//  用的是**当前 ANSI 代码页**，中文/日文路径会直接坏掉（打不开/找不到/串到别的文件）。
//  本库对外统一约定"路径是 UTF-8 std::string"，所以每个碰路径的地方都必须显式转换。
//
//  0.1.8 起统一走这里：io / ml / json / hash 都包含本头，不再各写一份。
//  （0.1.7 只在 io.cpp 内部实现过一份，于是 info()、sha256_file_hex()、sidecar 读取
//    这几处就漏掉了 —— 这正是"同一件事有两份实现"的典型代价。）
//
//  平台细节：
//    * Windows：MultiByteToWideChar(CP_UTF8) / WideCharToMultiByte(CP_UTF8)；
//    * Linux/macOS：std::string 本身就是字节序列，原样传递。
// =============================================================================

#include <filesystem>
#include <string>
#include <string_view>

#include "my_cpp_libs/export.hpp"

namespace my::core {

/// UTF-8 → path。空串返回空 path；不是合法 UTF-8 时退回 std::string 构造（不抛）。
MYLIB_API std::filesystem::path to_path(std::string_view utf8);

/// path → UTF-8。与 to_path 对称。
MYLIB_API std::string from_path(const std::filesystem::path& p);

}  // namespace my::core
