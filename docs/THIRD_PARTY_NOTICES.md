# 第三方依赖与许可证（随 SDK 包分发）

本包（`my_cpp_libs`）自身使用 **MIT License**，全文见同目录的 `my_cpp_libs-LICENSE`。

本包内**包含**下表中标为"随包分发"的第三方组件（头文件、静态库或动态库）。
它们的许可证原文一并放在本目录，`sources/` 子目录里是"许可证内嵌在源文件头部"
的那几个单头文件库的原始文件。

| 组件 | 版本 | 许可证 | 在包里的形态 | 许可证原文 |
|---|---|---|---|---|
| spdlog | 1.17.0 | MIT | 静态库 + 头文件 | `spdlog-LICENSE` |
| {fmt}（spdlog 内置） | bundled | MIT | 编进 spdlog 静态库 | `fmt-LICENSE.rst` |
| nlohmann/json | 3.12.0 | MIT | 头文件（`json.hpp` 的公共头要用） | `nlohmann-json-LICENSE.MIT` |
| Eigen | 5.0.0 | MPL-2.0 | 头文件（`math.hpp` 的公共头要用） | `eigen-COPYING.MPL2` + `eigen-COPYING.README` |
| cpp-httplib | 0.66.0 | MIT | 编进 `libmy_cpp_libs_http` | `cpp-httplib-LICENSE` |
| stb_image / stb_image_write | 2.32 / 1.18 | MIT（或公有领域，二选一） | 编进 `libmy_cpp_libs_image` + `libmylib_stb_impl` | `sources/stb_image.h`、`sources/stb_image_write.h`（声明在文件头） |
| pocketfft | git 4d865cb | BSD-3-Clause | 编进 `libmy_cpp_libs_dsp` | `sources/pocketfft_hdronly.h`（声明在文件头） |
| ONNX Runtime | 1.23.2（x64）/ 1.18.1（armv7）/ 1.19.2（aarch64） | MIT | 动态库（`lib/` 或 `bin/`）+ 导入库 | `onnxruntime-LICENSE`（+ `onnxruntime-ThirdPartyNotices.txt`） |
| OpenCV | 4.12.0 | Apache-2.0 | 动态库（`lib/` 或 `bin/`）+ 头文件 | `opencv-LICENSE` |
| ncnn（**不随包分发**） | 20250503 | BSD-3-Clause | 源码构建时才用；许可证原文仍随包附带 | `ncnn-LICENSE.txt` |

> 上表里没出现的库（例如本包没有启用 `cv` 时不会有 OpenCV）对应文件也不在包里。

## 再分发注意事项

1. **ONNX Runtime（MIT）/ OpenCV（Apache-2.0）**：再分发时必须随附各自的 `LICENSE` 与
   NOTICE（ONNX Runtime 的 `ThirdPartyNotices.txt` 已在包内；OpenCV 为 Apache-2.0，需要保留 NOTICE）。
   修改过的文件需要显著标注。
2. **MPL-2.0（Eigen）**：MPL 是**文件级** copyleft。本包只是以头文件形式分发 Eigen 未修改的
   源码，因此不触发源码公开义务；如果你修改了 Eigen 的文件，必须公开被修改的那些文件。
3. **MIT / BSD-3**：保留版权声明与许可证原文即可（本目录已提供）。
4. 打包脚本（`cmake/Packaging.cmake`）在缺任何许可证文件时会直接 `FATAL_ERROR`：
   缺许可证的包不应该存在。若遇到自编依赖没带 LICENSE，可在 `cmake/licenses/` 放兜底副本
   （ONNX Runtime 的 MIT LICENSE 与 ThirdPartyNotices 已随仓库提供）。

## 各组件的上游地址

| 组件 | 地址 |
|---|---|
| spdlog | <https://github.com/gabime/spdlog> |
| nlohmann/json | <https://github.com/nlohmann/json> |
| Eigen | <https://gitlab.com/libeigen/eigen> |
| cpp-httplib | <https://github.com/yhirose/cpp-httplib> |
| stb | <https://github.com/nothings/stb> |
| pocketfft | <https://gitlab.mpcdf.mpg.de/mtr/pocketfft> |
| ONNX Runtime | <https://github.com/microsoft/onnxruntime> |
| OpenCV | <https://github.com/opencv/opencv> |
| ncnn | <https://github.com/Tencent/ncnn> |

本项目的三方位图与版本锁定方式见仓库里的 `docs/01-规划与可行性.md` 与 `docs/03-构建与交叉编译.md`。
