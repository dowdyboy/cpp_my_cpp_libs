#pragma once
// =============================================================================
//  my::io —— 文件 / 目录 / 路径 / 临时文件
//
//  为什么需要它：这一层以前散落在四个地方自造轮子 ——
//    * json::save_file / image::save 各自拼路径、各自 fwrite；
//    * json 的落盘不是原子的，进程被 kill 会留下半个配置文件；
//    * 部署脚本和示例各自判断目录存在、各自 mkdir。
//  统一到这里之后，"跨平台 + 原子写 + 错误一律走 Status"只需要维护一份。
//
//  设计约定：
//    * 全部用 std::filesystem 的 **error_code 重载**，不抛异常
//      —— 这样 io 组件可以进 -fno-exceptions 子集；
//    * write_file 是**原子**的：先写同目录临时文件，再 rename 覆盖。
//      断电/被 kill 时要么是旧内容、要么是新内容，不会出现半截文件；
//    * 路径一律 UTF-8 std::string；Windows 上内部显式转 UTF-16 再交给文件系统，
//      所以中文/日文路径可用（0.1.7 之前这条其实没实现，只在文档里声称过）。
// =============================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "my_cpp_libs/core/status.hpp"
#include "my_cpp_libs/export.hpp"

namespace my::io {

struct FileInfo {
    std::uintmax_t size = 0;
    bool           is_regular   = false;
    bool           is_directory = false;
};

// ----------------------------------------------------------------------------- 查询
MYLIB_API bool      exists(std::string_view path) noexcept;
MYLIB_API bool      is_directory(std::string_view path) noexcept;
MYLIB_API Result<FileInfo> info(std::string_view path);

/// 递归列目录，返回**排好序**的条目名（不含 "." / ".."
MYLIB_API Result<std::vector<std::string>> list_dir(std::string_view path);

// ----------------------------------------------------------------------------- 读写
/// 一次性读整个文件（二进制安全）
MYLIB_API Result<std::string> read_file(std::string_view path);

/// 原子写：同目录临时文件 + rename。父目录必须已存在（不会自动创建）
MYLIB_API Status write_file(std::string_view path, std::string_view data);

/// 追加写（非原子，日志类场景够用）
MYLIB_API Status append_file(std::string_view path, std::string_view data);

// ----------------------------------------------------------------------------- 目录 / 元操作
MYLIB_API Status create_directories(std::string_view path);
/// 删除文件或**空**目录
MYLIB_API Status remove(std::string_view path);
/// 递归删除（不存在不算错误）
MYLIB_API Status remove_all(std::string_view path);
MYLIB_API Status rename(std::string_view from, std::string_view to);
MYLIB_API Status copy_file(std::string_view from, std::string_view to, bool overwrite = true);
/// 文件大小；不存在返回 NotFound
MYLIB_API Result<std::uintmax_t> file_size(std::string_view path);

// ----------------------------------------------------------------------------- 路径（纯字符串运算，不碰文件系统）
MYLIB_API std::string join(std::string_view a, std::string_view b);
MYLIB_API std::string parent(std::string_view path);
MYLIB_API std::string filename(std::string_view path);
/// 去掉扩展名的文件名（"a/b.json" -> "b"）
MYLIB_API std::string stem(std::string_view path);
/// 扩展名，带点（"a/b.json" -> ".json"）；没有则空串
MYLIB_API std::string extension(std::string_view path);
MYLIB_API std::string current_dir();
MYLIB_API Result<std::string> absolute(std::string_view path);

// ----------------------------------------------------------------------------- 临时文件
/// RAII 临时文件：构造时真的建出来（占位防撞名），析构时删除。
/// release() 可以把文件"交出去"（析构不再删）。
class TempFile {
public:
    TempFile() = default;
    TempFile(TempFile&& other) noexcept;
    TempFile& operator=(TempFile&& other) noexcept;
    TempFile(const TempFile&)            = delete;
    TempFile& operator=(const TempFile&) = delete;
    ~TempFile();

    /// dir 为空表示当前目录；ext 建议带上点（".tmp"）
    MYLIB_API static Result<TempFile> create(std::string_view dir = {},
                                             std::string_view prefix = "mylib_",
                                             std::string_view ext    = ".tmp");

    const std::string& path() const noexcept { return path_; }
    MYLIB_API Status   write(std::string_view data) const;
    MYLIB_API Status   append(std::string_view data) const;

    /// 放弃所有权：返回路径，析构不再删除
    MYLIB_API std::string release();
    /// 立即删除并重置为空
    MYLIB_API void reset() noexcept;

private:
    std::string path_;
};

}  // namespace my::io
