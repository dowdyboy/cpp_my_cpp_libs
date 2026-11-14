#include "my_cpp_libs/core/build_info.hpp"

#include <cstdio>
#include <string>

#include "my_cpp_libs/build_info_config.h"

#ifndef MYLIB_VERSION_STR
#  define MYLIB_VERSION_STR "0.0.0"
#endif

namespace my {
namespace {

constexpr const char* os_name_impl() noexcept {
#if defined(MYLIB_OS_WINDOWS)
    return "windows";
#elif defined(MYLIB_OS_MACOS)
    return "macos";
#elif defined(MYLIB_OS_LINUX)
    return "linux";
#else
    return "unknown";
#endif
}

constexpr const char* arch_name_impl() noexcept {
#if defined(MYLIB_ARCH_X86_64)
    return "x86_64";
#elif defined(MYLIB_ARCH_AARCH64)
    return "aarch64";
#elif defined(MYLIB_ARCH_ARM)
    return "arm";
#elif defined(MYLIB_ARCH_X86)
    return "x86";
#else
    return "unknown";
#endif
}

}  // namespace

const char* BuildInfo::libraryVersion() noexcept { return MYLIB_VERSION_STR; }
const char* BuildInfo::buildType() noexcept { return MYLIB_BUILD_TYPE_STR; }
const char* BuildInfo::compiler() noexcept { return MYLIB_COMPILER_STR; }
const char* BuildInfo::osName() noexcept { return os_name_impl(); }
const char* BuildInfo::archName() noexcept { return arch_name_impl(); }
const char* BuildInfo::targetTriple() noexcept { return MYLIB_TARGET_TRIPLE_STR; }
bool BuildInfo::isCrossCompiled() noexcept { return MYLIB_CROSS_COMPILED != 0; }
int BuildInfo::pointerBits() noexcept { return MYLIB_PTR_SIZE; }

const char* BuildInfo::summary() noexcept {
    static const std::string text = [] {
        std::string s;
        s += "my_cpp_libs ";
        s += MYLIB_VERSION_STR;
        s += " | ";
        s += MYLIB_TARGET_TRIPLE_STR;
        s += " | ";
        s += MYLIB_COMPILER_STR;
        s += " | ";
        s += MYLIB_BUILD_TYPE_STR;
        if (MYLIB_CROSS_COMPILED) s += " | cross";
        s += " | ptr=";
        s += std::to_string(MYLIB_PTR_SIZE);
        return s;
    }();
    return text.c_str();
}

}  // namespace my
