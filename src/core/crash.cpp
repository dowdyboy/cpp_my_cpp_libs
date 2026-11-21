// =============================================================================
//  my::crash 实现 —— 崩溃现场捕获（异步信号安全）
//
//  文件结构（自下而上）：
//      TextBuf       定长文本缓冲：格式化不分配、不加锁，写满即截断
//      raw_write     唯一输出通道：POSIX write(2) / Windows WriteFile
//      write_*       信号上下文里可调用的输出片段（头、信号行、栈、尾）
//      处理器        crash_on_signal / crash_on_terminate / crash_on_unhandled_exception
//      enable_*      每项处理器独立记账（"是否由我们接管" + "接管前是谁"），
//                    这样重复 install()、以及"装→不装→装"都不会记错旧处理器
//      install/uninstall、backtrace_text（普通上下文，允许分配）
//
//  输出一律 ASCII（中文只出现在注释里）：崩溃文本经常被重定向进日志系统、跨机器
//  解码，ASCII 最不容易变成乱码 —— 恰恰是崩溃时最不需要的意外。
//
//  关于 _Exit(3)：处理器写完故意不 re-raise，直接 _Exit(3)。
//    * re-raise 会让"我们自己"再被信号打一次，栈已损坏时可能反复触发；
//    * 不 re-raise 就不会产生 core 文件 —— 这是明确的取舍，见 crash.hpp 里的说明，
//      生产环境要同时开 core dump / WER dump。
// =============================================================================

// <signal.h> 里的 POSIX 声明（sigaction / sigaltstack / SA_ONSTACK / sigemptyset）
// 在 -std=c++17（严格 ANSI）下会被 glibc 藏起来。libstdc++ 一般已经替我们定义过
// _GNU_SOURCE，但本文件不依赖那个巧合 —— 特性宏必须在任何 include 之前定义。
#if !defined(_WIN32) && !defined(_GNU_SOURCE) && !defined(_DEFAULT_SOURCE) && \
    !defined(_POSIX_C_SOURCE) && !defined(_XOPEN_SOURCE)
#  define _DEFAULT_SOURCE 1
#endif

#include "my_cpp_libs/core/crash.hpp"

#include <cstddef>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include "my_cpp_libs/core/build_info.hpp"

// 平台头只能出现在这个 TU 里：crash.hpp 是对外头，不能被 windows.h 的宏污染。
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <time.h>
#  include <unistd.h>
#  if defined(__has_include)
#    if __has_include(<execinfo.h>)
#      define MYLIB_CRASH_HAVE_EXECINFO 1
#    endif
#    if __has_include(<cxxabi.h>)
#      define MYLIB_CRASH_HAVE_CXXABI 1
#    endif
#  elif defined(__GLIBC__) || defined(__APPLE__) || defined(__FreeBSD__)
#    define MYLIB_CRASH_HAVE_EXECINFO 1
#    define MYLIB_CRASH_HAVE_CXXABI 1
#  endif
#  if defined(MYLIB_CRASH_HAVE_EXECINFO)
#    include <execinfo.h>  // backtrace / backtrace_symbols / backtrace_symbols_fd
#  endif
#  if defined(MYLIB_CRASH_HAVE_CXXABI)
#    include <cxxabi.h>  // __cxa_demangle：只给 backtrace_text 用（普通上下文）
#  endif
#endif

// 能不能取调用栈：POSIX 看 execinfo，Windows 始终有 CaptureStackBackTrace。
#if defined(MYLIB_CRASH_HAVE_EXECINFO) || defined(_WIN32)
#  define MYLIB_CRASH_HAVE_BACKTRACE 1
#endif

namespace my {
namespace crash {
namespace {

// -----------------------------------------------------------------------------
//  编译期常量
// -----------------------------------------------------------------------------
/// 预格式化崩溃头的容量。静态缓冲，install() 写完就只读；写满即截断（宁可丢掉
/// exe 后半段，也不能因为格式化本身在信号上下文里再崩一次）。
constexpr std::size_t kHeaderCap = 1024;

/// 帧数硬上限。处理器会在（可能已损坏的）栈上开 void*[128] = 1 KiB 的数组，
/// 再大就更容易二次崩溃。
constexpr int kMaxFramesLimit = 128;

/// 备用信号栈（POSIX）：栈溢出（深递归/超大栈帧）时正常栈已经没有空间执行处理器，
/// 内核会把处理器打到这块独立栈上，输出才有机会写完。
/// 16 KiB 的依据：处理器峰值约 2~3 KiB（128 个帧指针 + 几层 256~512 B 格式化缓冲），
/// 留了约 5 倍余量。注意这是 .bss：占 RAM 不占镜像，嵌入式上别再放大。
#if !defined(_WIN32) && defined(SA_ONSTACK)
constexpr std::size_t kAltStackSize = 16u * 1024u;
alignas(16) unsigned char g_alt_stack[kAltStackSize];
#endif

// -----------------------------------------------------------------------------
//  TextBuf —— 定长文本缓冲
//
//  只在栈上/静态区使用：append 到容量为止，永不分配、永不加锁。
//  永远预留 1 字节给 '\0'，所以 c_str() 总是可安全用于 WriteFile/字符串字面量场景。
// -----------------------------------------------------------------------------
class TextBuf {
public:
    TextBuf(char* data, std::size_t cap) noexcept : data_(data), cap_(cap), len_(0) {}

    void put(const char* s) noexcept {
        if (s == nullptr) return;
        while (*s != '\0') push(*s++);
    }

    void put(char c) noexcept { push(c); }

    void put_n(const char* s, std::size_t n) noexcept {
        if (s == nullptr) return;
        for (std::size_t i = 0; i < n; ++i) push(s[i]);
    }

    /// 十进制无符号
    void put_uint(unsigned long long v) noexcept {
        char        tmp[20];
        std::size_t n = 0;
        do {
            tmp[n++] = static_cast<char>('0' + static_cast<int>(v % 10u));
            v /= 10u;
        } while (v != 0);
        while (n > 0) push(tmp[--n]);
    }

    void put_int(long long v) noexcept {
        if (v < 0) {
            push('-');
            // 先 +1 再取反：LLONG_MIN 直接取反是未定义行为
            put_uint(static_cast<unsigned long long>(-(v + 1)) + 1u);
        } else {
            put_uint(static_cast<unsigned long long>(v));
        }
    }

    /// 小写十六进制，至少 min_digits 位（左补 0）
    void put_hex(unsigned long long v, int min_digits) noexcept {
        char        tmp[16];
        std::size_t n = 0;
        do {
            const int d = static_cast<int>(v & 0xFu);
            tmp[n++]    = static_cast<char>(d < 10 ? ('0' + d) : ('a' + (d - 10)));
            v >>= 4;
        } while (v != 0);
        for (int i = static_cast<int>(n); i < min_digits && i < 16; ++i) push('0');
        while (n > 0) push(tmp[--n]);
    }

    /// 指针：定宽 "0x0012ab..."（宽度按指针位宽），便于肉眼对齐和 grep
    void put_pointer(const void* p) noexcept {
        put("0x");
        put_hex(static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p)),
                static_cast<int>(sizeof(void*) * 2u));
    }

    /// 帧号 "#000" / "#007" / "#100"（补到 3 位，100+ 帧时也能对齐）
    void put_frame_no(int index) noexcept {
        put('#');
        if (index < 100) push('0');
        if (index < 10) push('0');
        put_uint(static_cast<unsigned long long>(index < 0 ? 0 : index));
    }

    const char* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return len_; }

private:
    void push(char c) noexcept {
        // 留 1 字节给结尾 '\0'：cap_ 为 0 时什么都不写
        if (cap_ > 0 && len_ + 1 < cap_) data_[len_++] = c;
    }

    char*       data_;
    std::size_t cap_;
    std::size_t len_;
};

// -----------------------------------------------------------------------------
//  raw_write —— 信号上下文里唯一的输出手段
// -----------------------------------------------------------------------------
#if defined(_WIN32)
void raw_write(const char* s, std::size_t n) noexcept {
    if (s == nullptr || n == 0) return;
    // 每次都重新取句柄：调用方可能在 install() 之后 freopen 重定向了 stderr。
    // （WriteFile 对控制台句柄也成立：会按 ANSI 文本写出去。）
    const HANDLE h = ::GetStdHandle(STD_ERROR_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return;
    while (n > 0) {
        // 单次别超过 4 KiB：管道/控制台一次写入本来就有上限，分块后还能处理
        // "只写了一半"的情况；失败就放弃 —— 报错本身没有地方可报。
        const DWORD chunk = n > 0x1000u ? 0x1000u : static_cast<DWORD>(n);
        DWORD       written = 0;
        if (::WriteFile(h, s, chunk, &written, nullptr) == 0 || written == 0) return;
        s += written;
        n -= written;
    }
}
#else
void raw_write(const char* s, std::size_t n) noexcept {
    if (s == nullptr || n == 0) return;
    // write(2) 是 async-signal-safe 的；绝不能换成 fprintf/stdio —— 那会去抢
    // stdio 锁，崩溃时大概率死锁，ctest 只能看到 TIMEOUT。
    std::size_t off = 0;
    while (off < n) {
        const ssize_t w = ::write(STDERR_FILENO, s + off, n - off);
        if (w <= 0) return;  // EINTR/EAGAIN/EPIPE：放弃，不能重试到崩溃现场卡住
        off += static_cast<std::size_t>(w);
    }
}
#endif

void raw_write(const TextBuf& buf) noexcept { raw_write(buf.data(), buf.size()); }

// -----------------------------------------------------------------------------
//  进程级状态
//
//  只有 install()/uninstall() 会写这些变量（约定在单线程启动阶段调用），
//  处理器只读。所以不需要原子/锁 —— 处理器里也不能加锁。
// -----------------------------------------------------------------------------

/// 预格式化的崩溃头（install() 写，处理器只读）
char        g_header[kHeaderCap];
std::size_t g_header_len = 0;

/// 当前生效的配置（install() 已经把 max_frames 夹到 [1, kMaxFramesLimit]）
int  g_max_frames = 32;
bool g_print_backtrace = true;

/// install() 是否装过东西（uninstall() 的幂等判据）
bool g_installed = false;

/// 处理器重入守卫：处理器自己二次崩溃（栈被踩烂/backtrace 本身触发信号）时
/// 直接结束，绝不递归格式化。
volatile std::sig_atomic_t g_in_handler = 0;

#if defined(_WIN32)
/// SEH 过滤器：是否由我们接管 + 接管前是谁
bool                         g_uef_active   = false;
LPTOP_LEVEL_EXCEPTION_FILTER g_uef_previous = nullptr;

/// SIGABRT：同上
bool          g_abrt_active   = false;
void (*g_abrt_previous)(int) = nullptr;

/// install() 时刻的 tick（毫秒），只用来在崩溃头里报"崩溃离安装多久"
unsigned long long g_start_ticks_ms = 0;
#else
/// 每个信号的独立记账
struct HandlerSlot {
    bool             active = false;  // 是否由我们接管
    struct sigaction previous {};     // 接管前是谁（只在首次接管时写）
};

/// 接管的信号清单（名字在 signal_name() 里查，处理器只拿得到编号）
constexpr int kSignals[] = {
    SIGSEGV,
    SIGABRT,
    SIGFPE,
    SIGILL,
#ifdef SIGBUS
    SIGBUS,
#endif
};
constexpr std::size_t kSignalCount = sizeof(kSignals) / sizeof(kSignals[0]);

HandlerSlot g_slots[kSignalCount];

/// 备用信号栈的记账（与信号处理器分开，因为它是 per-thread 的）
#if defined(SA_ONSTACK)
bool    g_alt_stack_active = false;
stack_t g_alt_stack_previous {};
#endif

/// install() 时刻的单调时钟（毫秒）
unsigned long long g_start_ticks_ms = 0;
#endif  // _WIN32

/// std::terminate 的记账
bool          g_terminate_active = false;
void (*g_terminate_previous)()   = nullptr;

// -----------------------------------------------------------------------------
//  小工具（全部 async-signal-safe：只读常量、只算数）
// -----------------------------------------------------------------------------
const char* signal_name(int sig) noexcept {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGFPE:  return "SIGFPE";
        case SIGILL:  return "SIGILL";
#ifdef SIGBUS
        case SIGBUS:  return "SIGBUS";
#endif
        default:      return "unknown-signal";
    }
}

#if defined(_WIN32)
const char* exception_code_name(DWORD code) noexcept {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:    return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:      return "EXCEPTION_STACK_OVERFLOW";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:  return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION:    return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:       return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_BREAKPOINT:          return "EXCEPTION_BREAKPOINT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:  return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case 0xE06D7363u:                   return "C++ exception (MSVC EH)";
        default:                            return "unknown-exception";
    }
}
#endif

unsigned long current_pid_value() noexcept {
#if defined(_WIN32)
    return static_cast<unsigned long>(::GetCurrentProcessId());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

unsigned long long current_ticks_ms() noexcept {
#if defined(_WIN32)
#  if defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0600
    return static_cast<unsigned long long>(::GetTickCount64());
#  else
    // 老的 _WIN32_WINNT 下没有 GetTickCount64；GetTickCount 49.7 天回绕，
    // 换来的是老工具链也能编。回绕后差值可能为负，调用处会夹到 0。
    return static_cast<unsigned long long>(::GetTickCount());
#  endif
#else
    struct timespec ts;
    std::memset(&ts, 0, sizeof ts);
    if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;  // clock_gettime 是 async-signal-safe
    return static_cast<unsigned long long>(ts.tv_sec) * 1000ull +
           static_cast<unsigned long long>(ts.tv_nsec) / 1000000ull;
#endif
}

/// "崩溃离 install() 多少秒"，写成固定三位小数（处理器里不能用 snprintf）
void put_elapsed(TextBuf& buf) noexcept {
    const unsigned long long now  = current_ticks_ms();
    const unsigned long long diff = now >= g_start_ticks_ms ? now - g_start_ticks_ms : 0;
    buf.put_uint(diff / 1000ull);
    buf.put('.');
    const unsigned long long frac = diff % 1000ull;
    buf.put(static_cast<char>('0' + static_cast<int>(frac / 100ull)));
    buf.put(static_cast<char>('0' + static_cast<int>((frac / 10ull) % 10ull)));
    buf.put(static_cast<char>('0' + static_cast<int>(frac % 10ull)));
}

/// 可执行文件路径（install() 阶段取一次；拿不到就返回 0，头里显示 "?"）。
/// 用途：崩溃报告里能直接看出是哪一个二进制，省掉"用户到底跑的哪个版本"这轮猜测。
std::size_t exe_path_impl(char* out, std::size_t cap) noexcept {
    if (out == nullptr || cap < 2) return 0;
    out[0] = '\0';
#if defined(_WIN32)
    const DWORD n = ::GetModuleFileNameA(nullptr, out, static_cast<DWORD>(cap));
    if (n == 0) return 0;
    // n == cap 表示被截断（Vista+ 仍然保证 '\0' 结尾），前 cap-1 字节照写不误
    return n < cap ? static_cast<std::size_t>(n) : cap - 1;
#elif defined(__linux__)
    // /proc/self/exe 是最准的（argv[0] 可能是相对路径或被改过）。容器/嵌入式里
    // 没有 /proc 时返回 0 —— 只是少一行信息，不影响其它内容。
    const ssize_t n = ::readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) return 0;
    const std::size_t len = static_cast<std::size_t>(n);
    return len < cap - 1 ? len : cap - 1;
#else
    (void)cap;
    return 0;
#endif
}

// -----------------------------------------------------------------------------
//  预格式化崩溃头（只在 install() 里调用，普通上下文）
// -----------------------------------------------------------------------------
void build_header(const Options& options) noexcept {
    TextBuf b(g_header, sizeof g_header);

    b.put("my_cpp_libs crash:");
    if (options.write_build_info) {
        // 普通上下文，允许调用（summary 内部只构造一次字符串）；这是唯一能把
        // "一次崩溃"和"哪次构建"对上号的信息。
        b.put(" ");
        b.put(my::BuildInfo::summary());
    }
    b.put("\n");

    char              exe[512];
    const std::size_t exe_len = exe_path_impl(exe, sizeof exe);
    b.put("  exe      : ");
    if (exe_len > 0) {
        b.put_n(exe, exe_len);
    } else {
        b.put("?");
    }
    b.put("\n");

    b.put("  pid      : ");
    b.put_uint(static_cast<unsigned long long>(current_pid_value()));
    b.put("\n");

    b.put("  capture  : max_frames=");
    b.put_int(g_max_frames);
    b.put(" backtrace=");
    b.put(g_print_backtrace ? "on" : "off");
    b.put("\n");

    g_header_len = b.size();
}

// -----------------------------------------------------------------------------
//  信号上下文里的输出片段
// -----------------------------------------------------------------------------

/// 调用栈（信号上下文）。POSIX 走 backtrace + backtrace_symbols_fd（两者都不分配，
/// 见 backtrace(3) 的 async-signal-safe 说明）；Windows 走 CaptureStackBackTrace，
/// 只写地址 —— 符号化见 backtrace_text 上面的注释。
void write_backtrace_async() noexcept {
    if (!g_print_backtrace) return;

#if defined(MYLIB_CRASH_HAVE_EXECINFO)
    void*     frames[kMaxFramesLimit];
    const int captured = ::backtrace(frames, g_max_frames);

    char    label[128];
    TextBuf b(label, sizeof label);
    b.put("  backtrace: captured ");
    b.put_uint(static_cast<unsigned long long>(captured > 0 ? captured : 0));
    b.put(" frame(s), max=");
    b.put_int(g_max_frames);
    if (captured <= 0) {
        b.put(" (backtrace() failed; stack may be corrupted)\n");
    } else {
        b.put("\n");
    }
    raw_write(b);

    if (captured > 0) {
        // backtrace_symbols_fd 直接把 "模块(偏移) [地址]" 写进 fd，不 malloc；
        // 符号名交给离线工具解析
        ::backtrace_symbols_fd(frames, captured, STDERR_FILENO);
    }
#elif defined(_WIN32)
    void*        frames[kMaxFramesLimit];
    const USHORT captured =
        ::CaptureStackBackTrace(0, static_cast<DWORD>(g_max_frames), frames, nullptr);

    char    label[128];
    TextBuf b(label, sizeof label);
    b.put("  backtrace: captured ");
    b.put_uint(static_cast<unsigned long long>(captured));
    b.put(" frame(s), max=");
    b.put_int(g_max_frames);
    if (captured == 0) {
        b.put(" (CaptureStackBackTrace failed)\n");
    } else {
        b.put("\n");
    }
    raw_write(b);

    for (USHORT i = 0; i < captured; ++i) {
        char    line[96];
        TextBuf f(line, sizeof line);
        f.put("    ");
        f.put_frame_no(static_cast<int>(i));
        f.put(' ');
        f.put_pointer(frames[i]);
        f.put('\n');
        raw_write(f);
    }
#else
    static const char kUnavailable[] = "  backtrace: unavailable on this platform\n";
    raw_write(kUnavailable, sizeof kUnavailable - 1u);
#endif
}

/// 尾部提示（ASCII：崩溃现场最不该出现乱码）
void write_footer_async() noexcept {
    static const char kFooter[] =
        "  note     : best-effort capture; this handler does NOT produce a core dump.\n"
        "             symbolize the addresses offline with the matching symbols\n"
        "             (Linux: addr2line/llvm-symbolizer, Windows: PDB + llvm-symbolizer).\n";
    raw_write(kFooter, sizeof kFooter - 1u);
}

// -----------------------------------------------------------------------------
//  信号处理器（SIGSEGV / SIGABRT / SIGFPE / SIGILL / SIGBUS）
//
//  async-signal-safe：只用 raw_write（write/WriteFile）、backtrace*（不分配）、
//  栈上的 TextBuf，以及 _Exit。
// -----------------------------------------------------------------------------
[[noreturn]] void crash_on_signal(int sig) noexcept {
    if (g_in_handler != 0) std::_Exit(3);  // 重入：立刻结束，绝不递归格式化
    g_in_handler = 1;

    raw_write(g_header, g_header_len);

    char    line[160];
    TextBuf b(line, sizeof line);
    b.put("  signal   : ");
    b.put(signal_name(sig));
    b.put(" (");
    b.put_int(sig);
    b.put(")  t+");
    put_elapsed(b);
    b.put("s after install\n");
    raw_write(b);

    write_backtrace_async();
    write_footer_async();

    // 不 re-raise：见文件头。3 = "被 my::crash 捕获的崩溃"，CI 用它识别。
    std::_Exit(3);
}

// -----------------------------------------------------------------------------
//  std::terminate 处理器
//
//  terminate 可能在**异常展开过程中**被调用（栈上有未完成的 frame），也可能是
//  noexcept 逃逸、纯虚调用、线程构造失败。此时 current_exception()/typeid/
//  __cxa_demangle 都可能再次抛出或死锁，所以这里只做最保守的输出：
//  固定文本 + 地址，不去碰任何异常对象。
// -----------------------------------------------------------------------------
[[noreturn]] void crash_on_terminate() noexcept {
    if (g_in_handler != 0) std::_Exit(3);
    g_in_handler = 1;

    raw_write(g_header, g_header_len);

    // 缓冲要放得下两行（reason + hint，约 130 字节）：定长缓冲写满会静默截断，
    // 崩溃文本被截掉半句是最难查的那种"诊断本身有 bug"。
    char    line[256];
    TextBuf b(line, sizeof line);
    b.put("  reason   : std::terminate called  t+");
    put_elapsed(b);
    b.put("s after install\n");
    b.put("  hint     : uncaught exception / noexcept escape / pure virtual call\n");
    raw_write(b);

    write_backtrace_async();
    write_footer_async();

    std::_Exit(3);
}

#if defined(_WIN32)
// -----------------------------------------------------------------------------
//  SEH 未处理异常过滤器
//
//  只写"异常码 + 地址 + 线程 id + 栈地址"。**故意不调用 SymFromAddr/SymInitialize**：
//  它们要拿装载器锁、分配堆内存，崩溃上下文里调用可能死锁或抛二次异常。
//  符号化留给离线：用同一份构建的 exe/PDB（llvm-symbolizer --obj=app.exe）。
// -----------------------------------------------------------------------------
LONG WINAPI crash_on_unhandled_exception(EXCEPTION_POINTERS* info) noexcept {
    if (g_in_handler != 0) return EXCEPTION_EXECUTE_HANDLER;  // 二次崩溃：直接结束
    g_in_handler = 1;

    raw_write(g_header, g_header_len);

    char    line[512];
    TextBuf b(line, sizeof line);
    b.put("  reason   : unhandled SEH exception  t+");
    put_elapsed(b);
    b.put("s after install\n");

    const EXCEPTION_RECORD* er = (info != nullptr) ? info->ExceptionRecord : nullptr;
    if (er != nullptr) {
        b.put("  code     : 0x");
        b.put_hex(static_cast<unsigned long long>(er->ExceptionCode), 8);
        b.put(" ");
        b.put(exception_code_name(er->ExceptionCode));
        b.put("\n  address  : ");
        b.put_pointer(reinterpret_cast<const void*>(er->ExceptionAddress));
        b.put("\n  thread   : ");
        b.put_uint(static_cast<unsigned long long>(::GetCurrentThreadId()));

        // 访问冲突额外给"读/写/执行 + 访问地址"：这是区分空指针解引用、野指针写、
        // 执行不可执行页最快的一条信息。
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
            er->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) {
            if (er->NumberParameters >= 2) {
                const ULONG_PTR op = er->ExceptionInformation[0];
                b.put("\n  access   : ");
                b.put(op == 0 ? "read" : (op == 1 ? "write" : (op == 8 ? "execute" : "unknown")));
                b.put(" at ");
                b.put_pointer(reinterpret_cast<const void*>(er->ExceptionInformation[1]));
            }
        }
        b.put("\n");
    }
    raw_write(b);

    write_backtrace_async();
    write_footer_async();

    g_in_handler = 0;

    // 交回"安装前"的过滤器：Windows 上那通常是 WER 的默认处理，dump 仍然会产出 ——
    // 崩溃诊断不该以牺牲 core dump 为代价。没有旧过滤器时返回 CONTINUE_SEARCH，
    // 让 ntdll 的默认处理继续跑完（同样是为了 dump）。
    // 只有"我们自己的过滤器"被重复安装时才不能回交（会死循环），用指针比较挡掉。
    if (g_uef_previous != nullptr && g_uef_previous != &crash_on_unhandled_exception) {
        return g_uef_previous(info);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif  // _WIN32

// -----------------------------------------------------------------------------
//  处理器安装/还原（每项独立记账，天然幂等）
// -----------------------------------------------------------------------------

/// std::set_terminate
void enable_terminate(bool enable) noexcept {
    if (enable) {
        if (g_terminate_active) return;  // 已经是我们的，不重复保存"旧处理器"
        g_terminate_previous = std::set_terminate(&crash_on_terminate);
        g_terminate_active   = true;
    } else if (g_terminate_active) {
        if (g_terminate_previous != nullptr && g_terminate_previous != &crash_on_terminate) {
            (void)std::set_terminate(g_terminate_previous);
        }
        g_terminate_previous = nullptr;
        g_terminate_active   = false;
    }
}

#if defined(_WIN32)
void enable_signals(bool enable) noexcept {
    // --- SIGABRT（CRT 信号）：abort() 和 assert 失败都会走到这里 ---
    if (enable) {
        void (*previous)(int) = std::signal(SIGABRT, &crash_on_signal);
        if (!g_abrt_active) {
            // 首次接管：把"接管前是谁"记下来，uninstall() 要原样还回去
            if (previous != SIG_ERR) {
                g_abrt_previous = previous;
                g_abrt_active   = true;
            }
        }
        // 已经接管过就只是重新确认一次（期间可能被外部改掉过）
    } else if (g_abrt_active) {
        (void)std::signal(SIGABRT, g_abrt_previous);
        g_abrt_previous = nullptr;
        g_abrt_active   = false;
    }

    // --- SEH 未处理异常 ---
    if (enable) {
        LPTOP_LEVEL_EXCEPTION_FILTER previous =
            ::SetUnhandledExceptionFilter(&crash_on_unhandled_exception);
        if (!g_uef_active) {
            g_uef_previous = previous;
            g_uef_active   = true;
        }
    } else if (g_uef_active) {
        (void)::SetUnhandledExceptionFilter(g_uef_previous);
        g_uef_previous = nullptr;
        g_uef_active   = false;
    }
}

void enable_alt_stack(bool enable) noexcept { (void)enable; }  // Windows 没有 sigaltstack
#else
void enable_signals(bool enable) noexcept {
    for (std::size_t i = 0; i < kSignalCount; ++i) {
        HandlerSlot& slot = g_slots[i];

        if (!enable) {
            if (slot.active) {
                (void)::sigaction(kSignals[i], &slot.previous, nullptr);  // 原样还回去
                slot.active = false;
            }
            continue;
        }

        if (!slot.active) {
            // 先问出"接管前是谁"。问不到就不接管这一项 —— 装不上不能把旧处理器弄丢。
            if (::sigaction(kSignals[i], nullptr, &slot.previous) != 0) continue;
            slot.active = true;
        }

        struct sigaction sa;
        std::memset(&sa, 0, sizeof sa);
        sa.sa_handler = &crash_on_signal;
        (void)::sigemptyset(&sa.sa_mask);
        // SA_RESETHAND：处理器自己二次崩溃时回到默认动作（段错误 + core dump），
        // 而不是再进一次我们的处理器。_Exit(3) 才是正常出口，这个标志只管"出错时"。
#ifdef SA_RESETHAND
        sa.sa_flags |= SA_RESETHAND;
#endif
        // SA_ONSTACK：栈溢出场景下用备用信号栈执行处理器（见 g_alt_stack）
#ifdef SA_ONSTACK
        sa.sa_flags |= SA_ONSTACK;
#endif
        (void)::sigaction(kSignals[i], &sa, nullptr);
    }
}

void enable_alt_stack(bool enable) noexcept {
#if defined(SA_ONSTACK)
    if (enable) {
        if (g_alt_stack_active) return;
        stack_t current;
        std::memset(&current, 0, sizeof current);
        if (::sigaltstack(nullptr, &current) != 0) return;  // 平台不支持/查询失败：算了
        g_alt_stack_previous = current;

        stack_t ours;
        std::memset(&ours, 0, sizeof ours);
        ours.ss_sp    = g_alt_stack;
        ours.ss_size  = sizeof g_alt_stack;
        ours.ss_flags = 0;
        if (::sigaltstack(&ours, nullptr) != 0) return;
        g_alt_stack_active = true;
    } else if (g_alt_stack_active) {
        // ss_flags == SS_DISABLE 的旧值会让这次调用把备用栈关掉，正好是"还原"
        (void)::sigaltstack(&g_alt_stack_previous, nullptr);
        std::memset(&g_alt_stack_previous, 0, sizeof g_alt_stack_previous);
        g_alt_stack_active = false;
    }
#else
    (void)enable;
#endif
}
#endif  // _WIN32

/// 预热 backtrace
///
/// glibc 的 backtrace() 第一次调用会 dlopen("libgcc_s.so.1") 去取 _Unwind_Backtrace，
/// 那是"分配内存 + 装载器锁"，正是信号上下文里最忌讳的两件事。所以 install() 时先
/// 踩一次，把它变成后续调用只走已加载的路径。
void warm_up_backtrace() noexcept {
#if defined(MYLIB_CRASH_HAVE_EXECINFO)
    if (!g_print_backtrace) return;
    void* warm[4];
    (void)::backtrace(warm, 4);
#else
    (void)0;
#endif
}

int clamp_frames(int max_frames) noexcept {
    if (max_frames < 1) return 1;
    if (max_frames > kMaxFramesLimit) return kMaxFramesLimit;
    return max_frames;
}

// -----------------------------------------------------------------------------
//  backtrace_text 的实现部分（普通上下文：允许分配/反混淆）
// -----------------------------------------------------------------------------
#if defined(MYLIB_CRASH_HAVE_CXXABI)
/// 把 backtrace_symbols 里的 mangled 名还原成可读形式。
/// 输入形如 "./app(_ZN3foo3barEv+0x12) [0x401234]"，取括号内 '+' 之前那段去解；
/// 解不出来就返回空串（调用方保留原样）。
/// 注意：__cxa_demangle 会 malloc，只能在非信号上下文调用。
std::string demangle_if_possible(const char* symbol) {
    if (symbol == nullptr) return {};
    const char* open = std::strchr(symbol, '(');
    if (open == nullptr) return {};
    const char* plus  = std::strchr(open, '+');
    const char* close = std::strchr(open, ')');
    if (plus == nullptr || close == nullptr || plus >= close) return {};

    const std::string mangled(open + 1, static_cast<std::size_t>(plus - open - 1));
    if (mangled.empty()) return {};

    int   status    = 0;
    char* demangled = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
    if (demangled == nullptr) return {};
    std::string out(demangled);
    std::free(demangled);  // __cxa_demangle 的结果必须 free
    return out;
}
#endif

#if defined(MYLIB_CRASH_HAVE_EXECINFO)
/// 符号列里的可读名：能反混淆就反混淆，否则原样用 backtrace_symbols 给的文本
const char* readable_symbol(const char* raw, std::string& scratch) {
#if defined(MYLIB_CRASH_HAVE_CXXABI)
    scratch = demangle_if_possible(raw);
    return scratch.empty() ? raw : scratch.c_str();
#else
    (void)scratch;
    return raw;
#endif
}
#endif  // MYLIB_CRASH_HAVE_EXECINFO

std::string backtrace_text_impl(int skip, int max_frames) {
#if !defined(MYLIB_CRASH_HAVE_BACKTRACE)
    (void)skip;
    (void)max_frames;
    return {};
#else
    if (skip < 0) skip = 0;
    const int want = clamp_frames(max_frames);

    std::string out;
    char        line[512];

#if defined(MYLIB_CRASH_HAVE_EXECINFO)
    // 抓 want + skip 帧，再丢掉前 skip 帧：这样 skip 之后仍然能拿满 want 帧
    void*     frames[kMaxFramesLimit];
    const int limit    = (want + skip < kMaxFramesLimit) ? (want + skip) : kMaxFramesLimit;
    const int captured = ::backtrace(frames, limit);
    if (captured <= 0) return out;

    // backtrace_symbols 会分配（一次性 malloc）；这里在正常上下文，允许
    char** symbols = ::backtrace_symbols(frames, captured);

    for (int i = skip; i < captured; ++i) {
        TextBuf b(line, sizeof line);
        b.put_frame_no(i);
        b.put(' ');
        b.put_pointer(frames[i]);
        if (symbols != nullptr) {
            std::string scratch;
            const char* name = readable_symbol(symbols[i], scratch);
            if (name != nullptr && name[0] != '\0') {
                b.put("  ");
                b.put(name);
            }
        }
        b.put('\n');
        out.append(b.data(), b.size());
    }
    if (symbols != nullptr) std::free(symbols);
#elif defined(_WIN32)
    // Windows 只给地址：符号化需要 DbgHelp 的 SymFromAddr，它内部要拿装载器锁、
    // 分配堆内存，既不适合信号上下文，也不适合"正在写崩溃报告"时再引入新依赖。
    // 离线符号化：llvm-symbolizer --obj=app.exe 0x...（PDB 和 exe 必须同一次构建）。
    // 帧号带上 skip，和 POSIX 分支的编号方式保持一致（#000 是调用者那一帧）。
    void*        frames[kMaxFramesLimit];
    const USHORT captured = ::CaptureStackBackTrace(static_cast<DWORD>(skip),
                                                    static_cast<DWORD>(want), frames, nullptr);
    for (USHORT i = 0; i < captured; ++i) {
        TextBuf b(line, sizeof line);
        b.put_frame_no(skip + static_cast<int>(i));
        b.put(' ');
        b.put_pointer(frames[i]);
        b.put('\n');
        out.append(b.data(), b.size());
    }
#endif
    return out;
#endif  // MYLIB_CRASH_HAVE_BACKTRACE
}

}  // namespace

// -----------------------------------------------------------------------------
//  公开 API
// -----------------------------------------------------------------------------
void install(const Options& options) {
    // 先更新配置并重建头：重复调用时这部分必须生效（"用新配置覆盖"）。
    g_max_frames      = clamp_frames(options.max_frames);
    g_print_backtrace = options.print_backtrace && backtrace_supported();
    g_start_ticks_ms  = current_ticks_ms();  // 崩溃头里的 "t+Ns" 从这次 install() 起算
    build_header(options);

    // 只有第一次安装才预热（重复安装没必要再 dlopen 一次）。
    if (!g_installed) warm_up_backtrace();

    enable_terminate(options.install_terminate_handler);
    enable_signals(options.install_signal_handlers);
    enable_alt_stack(options.install_signal_handlers);

    g_installed = true;
}

void uninstall() noexcept {
    if (!g_installed) return;  // 幂等

    // 先撤信号（崩溃随时可能发生，尽量缩短"头已经不用了但处理器还在"的窗口）
    enable_signals(false);
    enable_alt_stack(false);
    enable_terminate(false);

    g_installed = false;
}

bool backtrace_supported() noexcept {
#if defined(MYLIB_CRASH_HAVE_BACKTRACE)
    return true;
#else
    return false;
#endif
}

std::string backtrace_text(int skip, int max_frames) {
    // 分配失败不该把调用方打死 —— 调用方很可能正在写崩溃报告。
    // core 要能进 -fno-exceptions 子集，所以 try/catch 按"异常是否开启"裁剪
    // （和 build_info.cpp 里 summary() 的写法一致）。
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
        return backtrace_text_impl(skip, max_frames);
    } catch (...) {
        return std::string();
    }
#else
    return backtrace_text_impl(skip, max_frames);
#endif
}

}  // namespace crash
}  // namespace my
