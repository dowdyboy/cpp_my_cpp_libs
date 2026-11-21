// =============================================================================
//  my::crash 测试（套件 crash）
//
//  为什么不真的触发 SIGSEGV/SIGABRT：进程内没法安全验证"处理器接管了崩溃" ——
//  一旦处理器没装上（或自己写崩了），整个测试进程直接消失，后面的用例全丢，
//  ctest 那边只能看到一个 TIMEOUT 或者段错误退出码，连是哪条用例都看不出来。
//  真实的崩溃路径由 %TEMP% 下的独立子进程探针验证（install() 后 raise(SIGABRT)，
//  父进程检查 stderr），这里只覆盖"不该崩的部分"：
//    * backtrace_text() 在非信号上下文里可用、且 max_frames 真的限得住行数；
//    * install()/uninstall() 的幂等与状态机（装两次、卸两次、装→卸→装）。
// =============================================================================
#include "test_framework.hpp"

#include <my_cpp_libs/core/crash.hpp>

#include <cstddef>
#include <string>
#include <thread>

namespace {

/// 文本的行数（末尾没有换行也算一行）
std::size_t count_lines(const std::string& text) {
    if (text.empty()) return 0;
    std::size_t lines = 1;
    for (char c : text) {
        if (c == '\n') ++lines;
    }
    // 末尾换行不会多出一行内容
    if (text.back() == '\n') --lines;
    return lines;
}

/// 带 "#" 或 "0x" 地址的行数：至少 1 行说明真的拿到了栈帧
std::size_t count_address_lines(const std::string& text) {
    std::size_t n    = 0;
    std::size_t pos  = 0;
    while (pos < text.size()) {
        const std::size_t end  = text.find('\n', pos);
        const std::size_t stop = end == std::string::npos ? text.size() : end;
        const std::string line = text.substr(pos, stop - pos);
        if (line.find('#') != std::string::npos || line.find("0x") != std::string::npos) ++n;
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return n;
}

}  // namespace

MYTEST_SUITE("crash");

MYTEST_CASE(crash_backtrace_text_is_usable) {
    if (!my::crash::backtrace_supported()) {
        MYTEST_SKIP("当前编译器/平台没有 execinfo 或 CaptureStackBackTrace");
    }

    // 1) 默认参数下必须给出一段能看的栈
    const std::string text = my::crash::backtrace_text();
    CHECK(!text.empty());
    CHECK(count_address_lines(text) >= 1);

    // 2) max_frames 必须真的限住行数，不然 32/128 这些参数就是摆设
    const std::string few = my::crash::backtrace_text(0, 4);
    CHECK(!few.empty());
    CHECK(count_lines(few) <= 4);

    // 3) 越界参数要夹住（0 会被夹成 1，1000 会被夹成 128），不许崩
    CHECK(count_lines(my::crash::backtrace_text(0, 0)) <= 1);
    CHECK(count_lines(my::crash::backtrace_text(0, 1000)) <= 128);

    // 4) skip 只能减少帧，不能反而变多
    CHECK(count_lines(my::crash::backtrace_text(1, 8)) <= 8);

    // 5) 多线程里调用也要能用（backtrace_symbols 内部会分配，但不是线程绑定的）
    std::string from_thread;
    std::thread t([&from_thread]() { from_thread = my::crash::backtrace_text(0, 8); });
    t.join();
    CHECK(!from_thread.empty());
}

MYTEST_CASE(crash_install_uninstall_is_idempotent) {
    // 默认配置连装两次：第二次必须只更新配置，不能把"我们自己的处理器"当成
    // "安装前的处理器"存起来 —— 否则 uninstall() 还原出来的还是我们自己，
    // 之后测试框架的崩溃处理器就永久丢了。
    my::crash::install();
    my::crash::install();

    // 用另一套配置覆盖（关掉构建信息/栈、把帧数改小）：覆盖后必须仍然幂等
    my::crash::Options tuned;
    tuned.write_build_info = false;
    tuned.print_backtrace  = false;
    tuned.max_frames       = 4;
    my::crash::install(tuned);

    // 两项都关掉：走"什么都不接管"的路径，同样不许崩
    my::crash::Options none;
    none.install_signal_handlers    = false;
    none.install_terminate_handler  = false;
    my::crash::install(none);

    my::crash::uninstall();
    my::crash::uninstall();  // 第二次必须是 no-op（幂等）

    // 装→卸→再装→再卸：确认状态机没有"装过一次就再也卸不干净"
    my::crash::install();
    my::crash::uninstall();

    // 卸载之后 backtrace_text 仍然可用：它完全不依赖处理器
    if (my::crash::backtrace_supported()) {
        CHECK(!my::crash::backtrace_text().empty());
    }
}
