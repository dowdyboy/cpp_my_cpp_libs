// =============================================================================
//  stb_image / stb_image_write 的实现单元
//
//  整个工程里**只有这一个 TU** 定义 STB_*_IMPLEMENTATION，其余文件只包含声明。
//  它被编进独立的 target（mylib_stb_impl，见 src/CMakeLists.txt），并在那边
//  关掉告警：上游代码不满足 -Wall -Wextra -Wpedantic 的洁癖，但也不该为了
//  几个告警去改上游文件（改了就没法按 commit 换版本）。
//
//  关掉的解码器（STBI_NO_*）：PSD / PIC / PNM / HDR。
//  相机链路只需要 PNG / JPEG / BMP / TGA / GIF。
//
//  STBI_FAILURE_USERMSG：让 stbi_failure_reason() 返回人能看懂的话，
//  这些字符串会直接进 my::Status 的 message。
// =============================================================================

#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_HDR
#define STBI_FAILURE_USERMSG

// Windows 上 stb 默认用 fopen(path)，按 ANSI 代码页解释 UTF-8 路径 —— 中文/日文
// 目录会打不开。stb 2.32 自带 `_wfopen` 版本，打开这两个宏即可：
//   STBI_WINDOWS_UTF8  -> stbi_load/stbi_load_from_file 等接收 UTF-8 文件名
//   STBIW_WINDOWS_UTF8 -> stbi_write_png/jpg/... 接收 UTF-8 文件名
// 非 Windows 平台这两个宏无副作用。
#define STBI_WINDOWS_UTF8
#define STBIW_WINDOWS_UTF8

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
