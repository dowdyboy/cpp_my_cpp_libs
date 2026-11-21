#include "my_cpp_libs/io/io.hpp"

#include "my_cpp_libs/core/path_utf8.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#else
#    include <fcntl.h>
#    include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace my::io {

using my::core::from_path;
using my::core::to_path;
namespace {

Status from_ec(const std::error_code& ec, const char* what, std::string_view path) {
    if (!ec) return Ok();
    return Status(Errc::IoError, std::string(what) + " 失败: " + std::string(path) + "（" +
                                     ec.message() + "）");
}

/// 生成不冲突的临时文件名。毫秒时间戳 + 进程 ID + 进程内自增：
/// 只靠进程内自增无法避免两个进程在同一毫秒撞名（write_file 的临时名因此会互相覆盖）。
std::string temp_suffix() {
    static std::atomic<std::uint64_t> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    return std::to_string(ms) + "-" + std::to_string(pid) + "-" +
           std::to_string(counter.fetch_add(1));
}

void remove_quiet(const std::string& path) noexcept {
    std::error_code ec;
    fs::remove(to_path(path), ec);
}

#if !defined(_WIN32)
std::string errno_text(int e) {
    return std::string(std::strerror(e)) + " (errno=" + std::to_string(e) + ")";
}
#endif

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

/// 创建临时文件并写入 + fsync。独占创建：同名已存在时返回 AlreadyExists，
/// 调用方换一个名字重试。任何一步失败都会清掉半截临时文件。
Status write_new_sync(const std::string& path, std::string_view data) {
#if defined(_WIN32)
    const std::wstring wpath = to_path(path).native();
    HANDLE h = ::CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = ::GetLastError();
        if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS) {
            return Status(Errc::AlreadyExists, "临时文件已存在: " + path);
        }
        return Status(Errc::IoError,
                      "无法创建临时文件: " + path + "（GetLastError=" + std::to_string(e) + "）");
    }

    const char* p    = data.data();
    std::size_t left = data.size();
    while (left > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            left, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        if (!::WriteFile(h, p, chunk, &written, nullptr)) {
            const DWORD e = ::GetLastError();
            ::CloseHandle(h);
            remove_quiet(path);
            return Status(Errc::IoError,
                          "写入临时文件失败: " + path + "（GetLastError=" + std::to_string(e) + "）");
        }
        if (written == 0) {
            ::CloseHandle(h);
            remove_quiet(path);
            return Status(Errc::IoError, "写入临时文件失败（WriteFile 无进展）: " + path);
        }
        p += written;
        left -= written;
    }
    if (!::FlushFileBuffers(h)) {
        const DWORD e = ::GetLastError();
        ::CloseHandle(h);
        remove_quiet(path);
        return Status(Errc::IoError,
                      "刷新临时文件失败: " + path + "（GetLastError=" + std::to_string(e) + "）");
    }
    if (!::CloseHandle(h)) {
        remove_quiet(path);
        return Status(Errc::IoError, "关闭临时文件失败: " + path);
    }
    return Ok();
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
        if (errno == EEXIST) return Status(Errc::AlreadyExists, "临时文件已存在: " + path);
        return Status(Errc::IoError, "无法创建临时文件: " + path + "（" + errno_text(errno) + "）");
    }

    const char* p    = data.data();
    std::size_t left = data.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            const int e = errno;
            ::close(fd);
            remove_quiet(path);
            return Status(Errc::IoError, "写入临时文件失败: " + path + "（" + errno_text(e) + "）");
        }
        if (n == 0) {
            ::close(fd);
            remove_quiet(path);
            return Status(Errc::IoError, "写入临时文件失败（write 无进展）: " + path);
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        const int e = errno;
        ::close(fd);
        remove_quiet(path);
        return Status(Errc::IoError, "刷新临时文件失败: " + path + "（" + errno_text(e) + "）");
    }
    if (::close(fd) != 0) {
        remove_quiet(path);
        return Status(Errc::IoError, "关闭临时文件失败: " + path);
    }
    return Ok();
#endif
}

/// 把同目录临时文件原子替换为目标文件，并尽量把目录项也刷盘。
Status replace_sync(const std::string& tmp, const std::string& target) {
#if defined(_WIN32)
    const std::wstring wtmp    = to_path(tmp).native();
    const std::wstring wtarget = to_path(target).native();
    if (!::MoveFileExW(wtmp.c_str(), wtarget.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD e = ::GetLastError();
        return Status(Errc::IoError,
                      "原子替换失败: " + target + "（GetLastError=" + std::to_string(e) + "）");
    }
    return Ok();
#else
    if (::rename(tmp.c_str(), target.c_str()) != 0) {
        return Status(Errc::IoError, "原子替换失败: " + target + "（" + errno_text(errno) + "）");
    }
    // rename 的持久化还依赖父目录 fsync；失败不视为写失败（部分文件系统不支持）。
    std::string parent = from_path(to_path(target).parent_path());
    if (parent.empty()) parent = ".";
    int dfd = ::open(parent.c_str(), O_RDONLY
#    if defined(O_DIRECTORY)
                                       | O_DIRECTORY
#    endif
    );
    if (dfd >= 0) {
        (void)::fsync(dfd);
        ::close(dfd);
    }
    return Ok();
#endif
}

}  // namespace

// ----------------------------------------------------------------------------- 查询
bool exists(std::string_view path) {
    std::error_code ec;
    return fs::exists(to_path(path), ec) && !ec;
}

bool is_directory(std::string_view path) {
    std::error_code ec;
    return fs::is_directory(to_path(path), ec) && !ec;
}

Result<FileInfo> info(std::string_view path) {
    const fs::path p = to_path(path);
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
    fs::directory_iterator it(to_path(path), ec);
    // 构造失败（没读权限等）时 it == end，循环体一次都不进 —— 必须在这里就报错，
    // 否则"读不了"会被当成"空目录"返回 Ok。
    if (ec) {
        return Status(Errc::IoError,
                      "遍历目录失败: " + std::string(path) + "（" + ec.message() + "）");
    }
    const fs::directory_iterator end;
    for (;;) {
        if (it == end) break;
        names.push_back(from_path(it->path().filename()));
        it.increment(ec);
        // 最后一次 increment 出错时 it 会变成 end，循环条件不会再进；
        // 必须在这里检查，否则"读到一半失败"会被当成完整列表返回。
        if (ec) {
            return Status(Errc::IoError,
                          "遍历目录失败: " + std::string(path) + "（" + ec.message() + "）");
        }
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

    // 同目录临时文件 + 原子替换；临时名带进程 ID，并用 O_EXCL/CREATE_NEW 独占创建，
    // 两个进程同一毫秒写同一目标也不会互相覆盖半截文件。
    std::string tmp;
    Status      st;
    for (int attempt = 0; attempt < 8; ++attempt) {
        tmp = target + "." + temp_suffix() + ".tmp";
        st  = write_new_sync(tmp, data);
        if (st.code() == Errc::AlreadyExists) continue;
        break;
    }
    if (!st) return st;

    if (auto replaced = replace_sync(tmp, target); !replaced) {
        remove_quiet(tmp);
        return replaced;
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
        // "x" 模式是**独占创建**：两个进程同目录同前缀、恰好落在同一毫秒时，
        // 先建的赢，另一个创建失败并换名字重试。
        // 以前是 exists() 检查 + 普通创建，那是有竞态的（temp_suffix 的计数器只在进程内唯一）。
        std::FILE* probe = nullptr;
#    if defined(_WIN32)
        {
            // Windows 上必须走宽字符版，否则又回 ANSI 代码页
            const fs::path p = to_path(path);
            const std::wstring w = p.native();
            probe = ::_wfopen(w.c_str(), L"wbx");
        }
#    else
        probe = std::fopen(path.c_str(), "wbx");
#    endif
        if (probe == nullptr) continue;  // 撞名：换一个名字
        std::fclose(probe);
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
