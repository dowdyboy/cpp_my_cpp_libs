#pragma once
// =============================================================================
//  构建期信息 —— 交叉编译时用来确认"我到底编出了什么"
//
//  例：
//      my::BuildInfo::archName() -> "arm"
//      my::BuildInfo::osName()   -> "linux"
//      my::BuildInfo::targetTriple() -> "arm-linux-gnueabihf"
// =============================================================================

#include "my_cpp_libs/export.hpp"

namespace my {

struct BuildInfo {
    /// 库版本号，如 "0.1.0"
    static const char* libraryVersion() noexcept;

    /// 构建类型："Debug" / "Release" / ...
    static const char* buildType() noexcept;

    /// 编译器："msvc 19.44" / "gcc 13.3.0"
    static const char* compiler() noexcept;

    /// 目标操作系统："windows" / "linux" / "macos" / "unknown"
    static const char* osName() noexcept;

    /// 目标架构："x86_64" / "arm" / "aarch64" / "x86"
    static const char* archName() noexcept;

    /// 目标三元组，如 "arm-linux-gnueabihf"
    static const char* targetTriple() noexcept;

    /// 编译期是否在交叉编译
    static bool isCrossCompiled() noexcept;

    /// 指针位宽：32 / 64
    static int pointerBits() noexcept;

    /// 人类可读的一行摘要，便于贴进日志头部
    static const char* summary() noexcept;
};

}  // namespace my
