#pragma once
// =============================================================================
//  符号可见性 / 导出宏
//
//  静态库（默认）时 MYLIB_API 为空；
//  动态库构建时 Windows 走 __declspec，GCC/Clang 走 visibility 属性。
// =============================================================================

#if defined(MYLIB_SHARED)
#  if defined(_WIN32) || defined(__CYGWIN__)
#    if defined(MYLIB_BUILDING)
#      define MYLIB_API __declspec(dllexport)
#    else
#      define MYLIB_API __declspec(dllimport)
#    endif
#  elif defined(__GNUC__) || defined(__clang__)
#    define MYLIB_API __attribute__((visibility("default")))
#  else
#    define MYLIB_API
#  endif
#  define MYLIB_HAS_SHARED 1
#else
#  define MYLIB_API
#  define MYLIB_HAS_SHARED 0
#endif

#if defined(_MSC_VER)
#  define MYLIB_DEPRECATED(msg) [[deprecated(msg)]]
#else
#  define MYLIB_DEPRECATED(msg) [[deprecated(msg)]]
#endif
