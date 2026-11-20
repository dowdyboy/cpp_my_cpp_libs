#include "my_cpp_libs/io/io.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#else
#    include <string>
#endif

namespace fs = std::filesystem;

namespace my::io {
namespace {

/// UTF-8 → 文件系统路径。
///
/// Windows 上 `fs::path(std::string)` 走的是**当前 ANSI 代码页**，中文/日文路径会直接坏掉
/// （0.1.7 之前头文件声称"内部转宽字符"，其实并没有）。这里显式按 UTF-8 解成 UTF-16。
/// Linux 上 std::string 本身就是字节序列，原样用。
fs::path to_path(std::string_view s) {
#if defined(_WIN32)
    if (s.empty()) return fs::path();
    const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                           nullptr, 0);
    if (wlen <= 0) return fs::path(std::string(s));  // 不是合法 UTF-8：退回原样，至少不崩
    std::wstring w(static_cast<std::size_t>(wlen), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), wlen);
    return fs::path(w);
#else
    return fs::path(std::string(s));
#endif
}

/// 文件系统路径 → UTF-8。与 to_path 对称：否则 list_dir 返回的中文路径会是乱码。
std::string from_path(const fs::path& p) {
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

Status from_ec(const std::error_code& ec, const char* what, std::string_view path) {
    if (!ec) return Ok();
    return Status(Errc::IoError, std::string(what) + " 失败: " + std::string(path) + "（" +
                                     ec.message() + "）");
}

/// 生成不冲突的临时文件名。用"毫秒时间戳 + 进程内自增"而不是 PID：
/// 不依赖 <unistd.h>，MinGW/MSVC 都能编；同目录并发写靠自增区分。
std::string temp_suffix() {
    static std::atomic<std::uint64_t> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return std::to_string(ms) + "-" + std::to_string(counter.fetch_add(1));
}

Status write_raw(const std::string& path, std::string_view data, bool append) {
    std::ios::openmode mode = std::ios::binary | std::ios::out;
    mode |= append ? std::ios::app : std::ios::trunc;
    std::ofstream f(to_path(path), mode);
    if (!f) return Status(Errc::IoError, "无法打开文件写入: " + path);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.flush();
    if (!f) return Status(Errc::IoError, "写入文件失败: " + path);
    return Ok();
}

}  // namespace

// ----------------------------------------------------------------------------- 查询
bool exists(std::string_view path) noexcept {
    std::error_code ec;
    return fs::exists(to_path(path), ec) && !ec;
}

bool is_directory(std::string_view path) noexcept {
    std::error_code ec;
    return fs::is_directory(to_path(path), ec) && !ec;
}

Result<FileInfo> info(std::string_view path) {
    const fs::path p{std::string(path)};
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        return Status(Errc::NotFound, "路径不存在: " + std::string(path));
    }
    FileInfo out;
    out.is_regular   = fs::is_regular_file(p, ec);
    out.is_directory = fs::is_directory(p, ec);
    ec.clear();
    const auto sz = fs::file_size(p, ec);
    out.size      = ec ? 0 : static_cast<std::uintmax_t>(sz);
    return out;
}

Result<std::vector<std::string>> list_dir(std::string_view path) {
    std::error_code ec;
    if (!fs::is_directory(to_path(path), ec)) {
        return Status(Errc::NotFound, "目录不存在: " + std::string(path));
    }
    std::vector<std::string> names;
    for (fs::directory_iterator it(to_path(path), ec), end; it != end; it.increment(ec)) {
        if (ec) return Status(Errc::IoError, "遍历目录失败: " + std::string(path) + "（" + ec.message() + "）");
        names.push_back(from_path(it->path().filename()));
    }
    std::sort(names.begin(), names.end());
    return names;
}

// ----------------------------------------------------------------------------- 读写
Result<std::string> read_file(std::string_view path) {
    const std::string p(path);
    std::error_code   ec;
    if (!fs::is_regular_file(to_path(p), ec)) {
        return Status(Errc::NotFound, "文件不存在: " + p);
    }
    std::ifstream f(to_path(p), std::ios::binary);
    if (!f) return Status(Errc::IoError, "无法打开文件读取: " + p);

    std::string out;
    const auto  size = fs::file_size(to_path(p), ec);
    if (!ec) out.reserve(static_cast<std::size_t>(size));

    char   buf[64 * 1024];
    while (f) {
        f.read(buf, sizeof(buf));
        const auto got = f.gcount();
        if (got > 0) out.append(buf, static_cast<std::size_t>(got));
    }
    if (f.bad()) return Status(Errc::IoError, "读取文件失败: " + p);
    return out;
}

Status write_file(std::string_view path, std::string_view data) {
    if (path.empty()) return Status(Errc::InvalidArgument, "path 为空");
    const std::string target(path);
    const std::string tmp = target + "." + temp_suffix() + ".tmp";

    if (auto st = write_raw(tmp, data, /*append=*/false); !st) return st;

    std::error_code ec;
    fs::rename(to_path(tmp), to_path(target), ec);
    if (ec) {
        // Windows 的 rename 不覆盖已存在目标：先删再改名。
        // 这里有一个很小的窗口（删掉之后、改名之前），但比"直接 truncate 原文件"安全得多：
        // 至少断电时要么旧内容、要么新内容，不会留半截。
        std::error_code ignored;
        fs::remove(to_path(target), ignored);
        ec.clear();
        fs::rename(to_path(tmp), to_path(target), ec);
        if (ec) {
            std::error_code ignored2;
            fs::remove(to_path(tmp), ignored2);
            return from_ec(ec, "原子替换", target);
        }
    }
    return Ok();
}

Status append_file(std::string_view path, std::string_view data) {
    if (path.empty()) return Status(Errc::InvalidArgument, "path 为空");
    return write_raw(std::string(path), data, /*append=*/true);
}

// ----------------------------------------------------------------------------- 目录 / 元操作
Status create_directories(std::string_view path) {
    std::error_code ec;
    fs::create_directories(to_path(path), ec);
    return from_ec(ec, "创建目录", path);
}

Status remove(std::string_view path) {
    std::error_code ec;
    const bool      removed = fs::remove(to_path(path), ec);
    if (ec) return from_ec(ec, "删除", path);
    if (!removed) return Status(Errc::NotFound, "路径不存在: " + std::string(path));
    return Ok();
}

Status remove_all(std::string_view path) {
    std::error_code ec;
    fs::remove_all(to_path(path), ec);
    return from_ec(ec, "递归删除", path);
}

Status rename(std::string_view from, std::string_view to) {
    std::error_code ec;
    fs::rename(to_path(from), to_path(to), ec);
    return from_ec(ec, "重命名", from);
}

Status copy_file(std::string_view from, std::string_view to, bool overwrite) {
    std::error_code ec;
    fs::copy_file(to_path(from), to_path(to),
                  overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::none, ec);
    return from_ec(ec, "复制", from);
}

Result<std::uintmax_t> file_size(std::string_view path) {
    std::error_code ec;
    const auto      sz = fs::file_size(to_path(path), ec);
    if (ec) return Status(Errc::NotFound, "取文件大小失败: " + std::string(path));
    return static_cast<std::uintmax_t>(sz);
}

// ----------------------------------------------------------------------------- 路径
std::string join(std::string_view a, std::string_view b) {
    if (a.empty()) return std::string(b);
    if (b.empty()) return std::string(a);
    std::string out(a);
    const char last = out.back();
    if (last != '/' && last != '\\') out.push_back('/');
    const bool b_starts_sep = b.front() == '/' || b.front() == '\\';
    out.append(b.substr(b_starts_sep ? 1 : 0));
    return out;
}

std::string parent(std::string_view path) { return from_path(to_path(path).parent_path()); }
std::string filename(std::string_view path) { return from_path(to_path(path).filename()); }
std::string stem(std::string_view path) { return from_path(to_path(path).stem()); }
std::string extension(std::string_view path) {
    return from_path(to_path(path).extension());
}

std::string current_dir() {
    std::error_code ec;
    const auto      p = fs::current_path(ec);
    if (ec) return {};
    return from_path(p);
}

Result<std::string> absolute(std::string_view path) {
    std::error_code ec;
    const auto      p = fs::absolute(to_path(path), ec);
    if (ec) return from_ec(ec, "求绝对路径", path);
    return from_path(p);
}

// ----------------------------------------------------------------------------- TempFile
TempFile::TempFile(TempFile&& other) noexcept : path_(std::move(other.path_)) {
    other.path_.clear();
}

TempFile& TempFile::operator=(TempFile&& other) noexcept {
    if (this != &other) {
        reset();
        path_ = std::move(other.path_);
        other.path_.clear();
    }
    return *this;
}

TempFile::~TempFile() { reset(); }

Result<TempFile> TempFile::create(std::string_view dir, std::string_view prefix,
                                  std::string_view ext) {
    std::string base(dir);
    if (base.empty()) base = current_dir();
    if (base.empty()) return Status(Errc::IoError, "取不到当前目录");

    TempFile out;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const std::string name = std::string(prefix) + temp_suffix() + std::string(ext);
        const std::string path = join(base, name);
        std::error_code   ec;
        if (fs::exists(to_path(path), ec)) continue;  // 撞名，换一个
        // 真的建出来占位：否则两个进程可能拿到同一个名字
        std::ofstream f(to_path(path), std::ios::binary);
        if (!f) return Status(Errc::IoError, "无法创建临时文件: " + path);
        out.path_ = path;
        return out;
    }
    return Status(Errc::IoError, "临时文件名连续冲突，放弃");
}

Status TempFile::write(std::string_view data) const {
    if (path_.empty()) return Status(Errc::NotInitialized, "临时文件未创建");
    return write_file(path_, data);
}

Status TempFile::append(std::string_view data) const {
    if (path_.empty()) return Status(Errc::NotInitialized, "临时文件未创建");
    return append_file(path_, data);
}

std::string TempFile::release() {
    std::string out = std::move(path_);
    path_.clear();
    return out;
}

void TempFile::reset() noexcept {
    if (path_.empty()) return;
    std::error_code ec;
    fs::remove(to_path(path_), ec);
    path_.clear();
}

}  // namespace my::io
