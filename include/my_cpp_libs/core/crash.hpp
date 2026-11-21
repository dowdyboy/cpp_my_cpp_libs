#pragma once
// =============================================================================
//  my::crash —— 崩溃现场捕获（best-effort）
//
//  解决的问题：进程被 SIGSEGV/SIGABRT/未处理异常带走时，默认只能看到
//  "程序异常退出"或者一串没人看得懂的地址。本模块在崩溃瞬间往 stderr 写一段
//  可以直接贴进 issue 的文本：哪次构建、哪个进程、哪个信号、哪些栈帧。
//
//  例（未捕获异常走到 std::terminate）：
//      my_cpp_libs crash: my_cpp_libs 0.1.9 | AMD64-Windows | GNU 15.2.0 | ...
//        exe      : D:\proj\app.exe
//        pid      : 12345
//        capture  : max_frames=32 backtrace=on
//        reason   : std::terminate called
//        backtrace: captured 7 frame(s), max=32
//        #000 0x00007ff6... 
//        #001 0x00007ff6... 
//        note     : best-effort capture; this handler does NOT produce a core dump.
//
// -----------------------------------------------------------------------------
//  能力边界（务必先读）
// -----------------------------------------------------------------------------
//  1) best-effort。处理器运行在**异步信号上下文**里，那里只允许调用 POSIX 列出的
//     async-signal-safe 函数；printf/iostream/malloc/std::string 都可能在锁上
//     死锁（ctest 里表现为一个 300 s TIMEOUT，什么线索都没有）。因此本模块：
//       * install() 阶段把构建信息/进程信息预格式化进**静态定长缓冲**；
//       * 处理器里只做 write(2)/WriteFile + backtrace，不分配、不加锁、不碰 stdio。
//  2) **不替代 core dump**。处理器写完就 _Exit(3)，故意不 re-raise，所以不会产生
//     core 文件；栈本身也已经坏了，寄存器/堆现场只有 core 里有。生产环境请同时开：
//       * Linux：ulimit -c unlimited + /proc/sys/kernel/core_pattern（coredumpctl）；
//       * Windows：WER LocalDumps，或 procdump -ma -e。
//     地址→符号必须用**同一份构建**产出的符号（Linux: addr2line / llvm-symbolizer；
//     Windows: PDB + llvm-symbolizer 或 dumpbin），所以符号一定要归档。
//  3) 处理器自己也可能失败（栈被踩烂、stderr 被关掉、信号来自别的线程）：最坏结果
//     是"和没装一样"，不会反过来把正常路径搞坏。
//  4) 不在正常路径上做任何事：没调 install() 就和本模块不存在一样。
//
//  符号化补充（POSIX）：backtrace_symbols_fd/backtrace_symbols 只能给出二进制里
//  导出的符号。可执行文件没导出符号时只剩 "app(+0x1234)"，两种解法：
//      * 编译/链接加 -rdynamic（-Wl,--export-dynamic），让动态符号表带上函数名；
//      * 或按地址离线解析：addr2line -f -C -e app 0x1234（.debug 信息在就行）。
//
//  平台分支：
//    * POSIX  ：sigaction 接管 SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGBUS（保存旧处理器，
//               uninstall() 原样还回去）+ 备用信号栈 sigaltstack（让"栈溢出"这种
//               崩溃也有地方执行处理器）+ execinfo 的 backtrace/backtrace_symbols_fd。
//    * Windows：SetUnhandledExceptionFilter 接管 SEH 未处理异常（保存旧过滤器，
//               写完诊断后**交回旧过滤器**，所以 WER dump 仍然会产出）+ SIGABRT。
//               SEH 路径只输出异常码/地址/线程 id/栈地址，不做符号化：SymFromAddr
//               要拿装载器锁、分配堆内存，在崩溃上下文里不能碰（用 PDB 离线符号化）。
//    * 两个平台都接管 std::terminate。
//
//  头文件里没有任何平台宏泄漏：windows.h/execinfo.h 都关在 crash.cpp 里。
// =============================================================================

#include <string>

#include "my_cpp_libs/export.hpp"

namespace my::crash {

/// 安装选项。install() 之前按需改；重复 install() 用新配置覆盖旧配置。
struct Options {
    /// POSIX：接管 SIGSEGV / SIGABRT / SIGFPE / SIGILL / SIGBUS；
    /// Windows：接管 SIGABRT（CRT 信号）+ SEH 未处理异常。
    /// false 时这些信号保持原样（由调用方/调试器/core dump 处理）。
    bool install_signal_handlers = true;

    /// 是否用 std::set_terminate 接管 std::terminate。
    /// 未捕获异常、noexcept 里逃出异常、纯虚调用、std::thread 构造失败都会走到
    /// terminate；不接管的话这些情况在 stderr 上通常只留下一句 "terminate called"。
    bool install_terminate_handler = true;

    /// 崩溃头里是否带 BuildInfo::summary()（版本 / 目标三元组 / 编译器 / 构建类型 /
    /// git commit / 指针位宽）。
    /// 关掉省不了多少字节，但它是唯一能把"一次崩溃"和"哪次构建"对上号的信息，
    /// 线上默认别关。
    bool write_build_info = true;

    /// 是否在崩溃头里追加调用栈
    /// （POSIX: backtrace + backtrace_symbols_fd；Windows: CaptureStackBackTrace）。
    /// 平台不支持时自动降级为 off（不会报错）。
    bool print_backtrace = true;

    /// 最多输出多少帧。实现里夹到 [1, 128]：值太大会让处理器在（可能已经损坏的）
    /// 栈上放一个更大的指针数组，更容易二次崩溃。
    int max_frames = 32;
};

/// 安装崩溃处理器（进程级、幂等）。
///
/// * 重复调用：用新配置覆盖旧配置，只重新预格式化崩溃头；已经存下来的
///   "安装前的处理器"不会被我们自己的处理器覆盖（否则 uninstall() 会还原错）。
///   从"装"切到"不装"再切回来也支持（每项处理器独立记账）。
/// * 调用时机：建议在 main() 开头、任何工作线程启动前调用。本函数不是线程安全的
///   （它读写进程级 sigaction / terminate handler），但不需要分配、不会失败：
///   装不上就当没装，不影响程序正常路径。
/// * 备用信号栈（POSIX）是 per-thread 的：本模块只覆盖调用 install() 的那个线程
///   （通常是主线程）。其它线程要覆盖栈溢出，需要各自 sigaltstack()。
MYLIB_API void install(const Options& options = {});

/// 恢复安装前的处理器（进程级、幂等）。没装过就什么都不做。
/// noexcept：可以在析构/fast-exit 路径上调用。
MYLIB_API void uninstall() noexcept;

/// 编译/链接期是否真的能取到调用栈
/// （POSIX: execinfo.h 可用；Windows: CaptureStackBackTrace 可用）。
MYLIB_API bool backtrace_supported() noexcept;

/// 在**非信号上下文**里取一段可读的调用栈文本（调试/日志用）。
///
/// @param skip       跳过最前面的 skip 帧（0 表示包含本函数自身）
/// @param max_frames 最多输出多少帧，夹到 [1, 128]
/// @return 每行形如 "#000 0x00007f... symbol"；取不到时返回空串 —— 这不是错误：
///         平台没有 execinfo、栈已被破坏、分配失败都会走到这里。
///
/// 注意：本函数**会分配内存**（backtrace_symbols / std::string），不能在信号处理器里
/// 调用；信号上下文请交给 install() 装好的处理器。
MYLIB_API std::string backtrace_text(int skip = 0, int max_frames = 32);

}  // namespace my::crash
