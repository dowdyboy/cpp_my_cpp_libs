# my_cpp_libs

[![CI](https://github.com/dowdyboy/cpp_my_cpp_libs/actions/workflows/ci.yml/badge.svg)](https://github.com/dowdyboy/cpp_my_cpp_libs/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![GitHub](https://img.shields.io/badge/repo-dowdyboy%2Fcpp__my__cpp__libs-181717?logo=github)](https://github.com/dowdyboy/cpp_my_cpp_libs)

> 仓库：<https://github.com/dowdyboy/cpp_my_cpp_libs> ｜ 许可证：[MIT](LICENSE)

跨平台 C++17 三方库统一封装层 + 调用演示工程。

一套源码，在 **Windows(x64/MSVC、MinGW)** 与 **Linux(x86_64、armv7-A/Zynq-7000、aarch64)** 上
用同一份 CMake 配置完成编译，业务代码不需要写平台分支。

```
业务代码  ->  my_cpp_libs::{core, io, time, thread, log, json, math, infer, image, dsp, ml, net, serial, http, cv}
          ->  spdlog / nlohmann / Eigen / ONNX Runtime / stb / pocketfft / cpp-httplib / OpenCV(可选)
```

## 这是什么

1. **二次封装库**：把 `resources/` 里收集的开源库收进统一 API，错误处理统一成 `my::Status` / `my::Result<T>`，
   换库、换平台时业务代码不动。
2. **调用演示工程**：`examples/` 里既有封装层的用法，也有各三方库**原生 API** 的常用写法对照，
   可以直接当速查手册用。

## 预编译 SDK（`dist/`）

全量包（含 ORT/OpenCV），拿到目录即可 `find_package`。**0.1.3 起由 CPack 生成**，历史版本见 GitHub Releases；
每个包都带 `share/licenses/`（我们的 MIT + 全部三方原始许可证 + `THIRD_PARTY_NOTICES.md`）。

| 包 | 平台/工具链 | 内含 |
|---|---|---|
| `my_cpp_libs-0.1.9-win-x64-msvc.zip` | Windows x64 / MSVC 19.5x | ORT 1.23.2 + OpenCV 4.12（DLL 在 `bin/`）；**不含 ncnn** |
| `my_cpp_libs-0.1.9-win-x64-mingw-basic.zip` | Windows x64 / MinGW GCC 15.2 | **基础包**：无 ORT/OpenCV（ABI 不兼容，见 Deps.cmake） |
| `my_cpp_libs-0.1.9-linux-armv7-glibc239.tar.gz` | armv7-A 硬浮点（2024 SDK，**glibc ≥ 2.38**） | ORT 1.18.1 + OpenCV 4.12 |
| `my_cpp_libs-0.1.9-linux-aarch64.tar.gz` | Pi 4B / 通用 aarch64（ARM GNU 14.2，glibc ≈ 2.40） | ORT 1.19.2 + OpenCV 4.12 |

```cmake
find_package(my_cpp_libs CONFIG REQUIRED)
target_link_libraries(app PRIVATE my_cpp_libs::all)
my_cpp_libs_copy_runtime_dlls(app)   # 仅 Windows
```

> **Zynq-7020 / PetaLinux 2020.2（glibc 2.30）怎么办**：上面那个 armv7 包是 2024 SDK
> （glibc 2.39）编的，**上不了 2.30 的板子**。两种做法：用配套 SDK 在 Linux 上
> `cmake --preset linux-armv7-sdk2020` 自己编，或者用全静态的
> `cmake --preset linux-armv7-static`。0.1.0 时手工打过 `glibc230` 包，那个产物不可复现
> （需要 PetaLinux SDK），所以没有随 0.1.2 重新生成。

### 自己重新打包（可复现）

**一条命令**（本机装齐四套工具链时，生成带 ORT/OpenCV 的全量包 + 逐个解包验证）：

```powershell
.\scripts\package-all.ps1          # 四个包 → dist/
.\scripts\verify-package.ps1 -Package dist\my_cpp_libs-0.1.9-win-x64-msvc.zip
```

CI 里也有两个 `package-*` job（`package-linux` / `package-windows`）跑同一套 CPack 流程，
但**只带 CI 能拿到的依赖**，所以包名带后缀说明内含：

| 包名 | 来源 | 内含 |
|---|---|---|
| `win-x64-msvc` | 本机 `package-all.ps1` | ORT + OpenCV（不含 ncnn，见下） |
| `win-x64-msvc-ort` | CI `package-windows` | ORT，无 OpenCV/ncnn |
| `linux-*-glibcNNN` | 本机 | ORT + OpenCV |
| `linux-*-glibcNNN-basic` | CI `package-linux` | 无 ORT/OpenCV（交叉预编译包不进 CI 依赖） |
| `win-x64-mingw-basic` | 本机/CI | 无 ORT/OpenCV（ABI 不兼容） |

> 包名后缀是**显式参数**而不是猜出来的：glibc 版本 CMake 推不出来，必须由构建者给，
> 否则很容易把"能编"误当成"能上板"。打包脚本在缺任何三方许可证时会直接
> `FATAL_ERROR`（`cmake/licenses/` 里是自编依赖的兜底副本）—— 缺许可证的包不会被生成。
>
> CI 的 `windows-msvc` 与 `package-windows` 会传 `fetch-deps.ps1 -RequireOrtWin`：
> 拿不到 ONNX Runtime 就让 job 失败，不再让 ml 静默 skip。官方 1.23.2 直链失效时，
> 在仓库变量 `MYLIB_ORT_WIN_URL` 里指向可用的 zip 即可（不要改脚本）。

## 快速开始

```powershell
# 0) 首次 clone 后补一次三方库压缩包（它们不进 git；离线环境手工放进 resources/ 也行）
.\scripts\fetch-deps.ps1          # Linux/WSL: bash scripts/fetch-deps.sh

# Windows / MinGW（最快）
cmake --preset win-mingw
cmake --build --preset win-mingw
ctest --preset win-mingw          # 按套件注册，能看出是哪一块挂了

# Windows / MSVC（唯一能用 ONNX Runtime 预编译包的方式）
cmake --preset win-msvc
cmake --build --preset win-msvc
ctest --preset win-msvc

# Windows / MinGW + UBSan（未定义行为检测，trap 模式，不需要 libubsan）
cmake --preset win-ubsan
cmake --build --preset win-ubsan
ctest --preset win-ubsan

# 板端图像链路（不需要 OpenCV）：合成图 → 编解码 → letterbox → 推理 → 框映射回原图
.\build\win-mingw\bin\example_image.exe

# 信号链路：合成 4 个突发 → 频谱/找峰 → 突发检测 → 调制识别 → FSK 解调
.\build\win-mingw\bin\example_signal.exe

# sklearn 模型（iris 逻辑回归）：加载 → 单样本 → 批量
.\build\win-msvc\bin\Release\example_ml.exe models\ml\iris_logreg.onnx

# 网络：TCP 回显 + 分帧 + UDP 往返（回环）
.\build\win-mingw\bin\example_net.exe

# 把本库嵌进自己的工程（独立工程，add_subdirectory 验证）
cmake -S examples\consumer_app -B build\consumer_app -G Ninja
cmake --build build\consumer_app
.\build\consumer_app\consumer_app.exe

# HTTP：板端 REST + 客户端调用（可加一个 URL 指向你的 Flask）
.\build\win-mingw\bin\example_http.exe

# 交叉编译到 armv7：三个预设各有明确用途，别混
cmake --preset linux-armv7          # 2024 SDK(glibc 2.39)：只做编译/链接验证
cmake --preset linux-armv7-static   # 全静态：板子 glibc 太旧时的逃生路线，可上板
cmake --preset linux-armv7-sdk2020  # 板子配套 SDK(glibc 2.30)：上板正解，需在 Linux 上配

# 六个预设一把梭（宿主平台会顺带跑 ctest）
.\scripts\build-all.ps1

# 本机跑一遍 CI 等价检查（默认 / UBSan / 最小配置）
.\scripts\ci-local.ps1

# 可选组件：ncnn 备选后端 / OpenCV 图像组件（win-msvc 预设默认已开）
cmake --preset win-msvc -DMYLIB_ENABLE_NCNN=ON -DMYLIB_ENABLE_CV=ON

# 生成一个最小 ONNX 模型，验证推理链路（仅 MSVC 构建可跑）
python scripts/make_test_model.py models/identity_1x1x4.onnx
.\build\win-msvc\bin\Release\example_infer.exe models\identity_1x1x4.onnx
```

产物在 `build/<preset>/bin/`，Linux 目标产出的是 ELF（拷贝到板子上即可运行）。

## 已实现的组件

| 组件 | target | 底层库 | 平台覆盖 | 说明 |
|---|---|---|---|---|
| core | `my_cpp_libs::core` | — | 全平台 | `Status` / `Result<T>` / 构建信息 / fp16 转换 / **SHA-256**（`my::hash`）/ **协议编解码**（CRC16/CRC32、hex、base64、COBS；`my::codec`）/ **崩溃现场捕获**（`my::crash`） |
| **io** | `my_cpp_libs::io` | `std::filesystem` | 全平台（含 armv7） | 文件/目录/路径/临时文件；**原子写**（临时文件 + rename）；不抛异常，进 `-fno-exceptions` 子集 |
| **time** | `my_cpp_libs::time` | `<chrono>` | 全平台（含 armv7） | 单调时钟（测耗时）、UTC 时间戳、`Stopwatch`/`ScopedTimer`；进 `-fno-exceptions` 子集 |
| **thread** | `my_cpp_libs::thread` | `<thread>` | 全平台（含 armv7） | 线程池、有界阻塞队列、周期任务；析构必 drain+join |
| log | `my_cpp_libs::log` | spdlog 1.17 | 全平台 | 分级、异步、滚动、自定义 sink、未初始化兜底 |
| json | `my_cpp_libs::json` | nlohmann/json 3.12 | 全平台 | 路径式访问 `a.b[0].c`、异常转 Status、结构体映射 |
| math | `my_cpp_libs::math` | Eigen 5.0 | 全平台 | 姿态转换、线性方程、最小二乘、统计/滤波 |
| infer | `my_cpp_libs::infer` | ONNX Runtime 1.23 / ncnn | **x64 + armv7 + aarch64** | 双后端可插拔；含预处理/后处理（归一化、letterbox、softmax、NMS） |
| dsp | `my_cpp_libs::dsp` | pocketfft | **全平台（含 armv7）** | 频谱/PSD、FIR/IIR、混频抽取、突发检测、调制识别、AM/FM/ASK/FSK/BPSK 解调；**流式（分块）处理**：`FirFilter`/`BiquadFilter`/`ComplexMixer`/`Decimator`/`Agc` 跨块保持状态 |
| ml | `my_cpp_libs::ml` | ONNX Runtime（模型由 skl2onnx 导出） | 随 infer（x64 + armv7） | sklearn 表格模型：双输出、特征顺序校验、类别名映射、批量推理；**不新增 C++ 三方库** |
| net | `my_cpp_libs::net` | —（裸 socket） | **全平台** | TCP 客户端/服务端、UDP 广播组播、超时、SIGPIPE 安全、长度前缀分帧；**`TcpServer`**（accept 循环 + 连接上限 + 空闲超时 + 每连接 handler，noex 构建里为桩） |
| **serial** | `my_cpp_libs::serial` | —（裸系统调用） | 全平台 | 串口：任意波特率(Windows)/标准档位(Linux)、超时读写、行读取、`flush`、端口枚举；PTY 上真跑收发；进 `-fno-exceptions` 子集 |
| http | `my_cpp_libs::http` | cpp-httplib 0.66 | 全平台 | HTTP 客户端 + 服务端、JSON 便捷接口、线程池可配；公共头不暴露 httplib |
| image | `my_cpp_libs::image` | stb_image 2.32 | **全平台（含 armv7）** | 解码/编码、缩放、letterbox→张量；**不依赖 OpenCV**，板端走这条 |
| cv | `my_cpp_libs::cv` | OpenCV 4.12（可选；CI 用 apt 的 4.6 验过） | x64 + **armv7**（`third_party/opencv-4.12.0-armv7`，随 `dist/` 的 armv7 包一起分发） | 图像 IO、letterbox 预处理、坐标逆变换 |

## 板端实测（Zynq-7020, Cortex-A9 @666MHz）

| 项目 | 结果 |
|---|---|
| 单元测试 | 宿主端：全组件 **398 条**（含 cv 12 条）、无 OpenCV **386 条**、no-exceptions 子集 **192 条**——**以 [docs/test-counts.md](docs/test-counts.md)（由 `mylib_tests --list` 生成）为准**；<br>板端 **158 / 158 通过（2026-11-16 上板实测，全组件，2 秒跑完）** ← 0.1.0 时的历史数据 |
| 板端性能（Zynq-7020） | ml 表格模型 ≈0.07~0.10 ms/次；dsp 全链路 67 ms；image 957 ms；net 36 ms；http 141 ms（见 [06 §0](docs/06-板端验证手册.md)） |
| 板端性能（Pi 4B / aarch64） | **158/158 通过（1 秒）**；ml 0.016~0.017 ms/次；bench_cnn 74 ms（比 A9 快 10.4×）（见 [06 §0.7](docs/06-板端验证手册.md)） |
| **全依赖上板** | ✅ 8 个三方库全部有 armv7 产物：Eigen / nlohmann / spdlog / stb / pocketfft（头文件或源码）+ ORT 1.18.1 + ncnn + **OpenCV 4.12（GLIBC_2.29）** |
| small_cnn（32×32） | 2.47 ms |
| bench_cnn（224×224, ≈300 MMAC） | **585 ms**（ncnn）/ **865 ms**（ORT）——两个数字来自不同构建/后端，测量条件见 [06 §0](docs/06-板端验证手册.md) |
| MobileNetV2-12（官方模型） | **1367 ms**（ORT） |
| 内存峰值 | 140~153 MB |
| 数值一致性 | 与 x86 一致 |

> 板子是 PetaLinux 2020.2（glibc 2.30 / GCC 9.2），用配套 SDK 交叉编译。
> ORT 需要 1.18.1（1.19+ 要求 GCC 11），已经编好放在
> `third_party/onnxruntime-armv7-petalinux2020`。详见 [板端验证手册](docs/06-板端验证手册.md)。

## 一个最小可用服务骨架

```powershell
# 配置（默认值 ← config.json ← 环境变量 ← 命令行）→ 日志 → 线程池 → 周期任务 → 原子写结果
.\build\win-mingw\bin\example_app_skeleton.exe
.\build\win-mingw\bin\example_app_skeleton.exe --worker.count=4 --worker.iterations=2000
$env:MYAPP__WORKER__COUNT=8; .\build\win-mingw\bin\example_app_skeleton.exe

# 板端（armv7 静态版，不依赖任何 .so）
./example_app_skeleton --output.path=/tmp/result.json
```

见 [基础组件文档](docs/11-基础组件.md)。

## 命令行工具

```powershell
# 模型体检：能不能加载？输入输出长什么样？跑一次看看？延迟多少？
model_inspect models\small_cnn.onnx --run --bench 100

# ncnn 转换（在 WSL 里编工具，然后批量转换）
wsl -d Ubuntu-24.04 -u root -- bash scripts/build-ncnn-tools-wsl.sh
wsl -d Ubuntu-24.04 -u root -- bash scripts/convert_all_models.sh

# 跨后端数值比对（ORT vs ncnn）
model_inspect models\small_cnn.onnx       --run --dump output\a.txt
model_inspect models\ncnn\small_cnn.param --run --shape 1,3,32,32 --dump output\b.txt
python tools\compare_outputs.py output\a.txt output\b.txt

# 板端部署（板子到手后）。默认 -Preset linux-armv7-static：全静态、不依赖板端 glibc，
# PetaLinux 2020.2（glibc 2.30）可直接跑；要用板子配套 SDK 的动态产物时加 -Preset linux-armv7-sdk2020
.\scripts\deploy-and-bench.ps1 -Board <ip> -User root
# 板子不在手边：先打离线包（03 §5 讲了怎么手工放上去）
.\scripts\deploy-and-bench.ps1 -Package
# ASan/UBSan 检查（WSL）
wsl -d Ubuntu-24.04 -u root -- bash scripts/run-tests-asan-wsl.sh
```

`build/<preset>/bin/model_inspect`，见 [推理方案](docs/04-推理方案与模型兼容性.md)。

## 目录结构

```
CMakeLists.txt          顶层：选项、平台探测、安装
CMakePresets.json       10 个 configure 预设（Windows 可见 7 个，Linux 另 3 个）
cmake/
  Platform.cmake        架构/平台宏、统一 target 工厂
  Deps.cmake            三方库挂载（自动解压 resources/*.zip）
  toolchains/           交叉编译工具链文件
include/my_cpp_libs/    对外公共头
src/                    组件实现
examples/               封装层示例 + 原生 API 示例 + 独立消费方工程
tools/                  命令行工具（model_inspect）+ 模型生成脚本 + sklearn 导出工具
tests/                  无依赖的单元测试（条数见 docs/test-counts.md；另有 no-exceptions 子集）
models/                 测试模型（脚本生成：7 种 ONNX 形态 + ml/ 下的 sklearn 演示模型与 golden）
scripts/                构建脚本、ONNX Runtime / OpenCV 交叉编译脚本
third_party/            由 resources/*.zip 自动解压（不入库）
docs/                   规划、API、构建、推理、OpenCV、板端验证、图像链路、信号处理
```

## 文档

- [规划与可行性](docs/01-规划与可行性.md) —— 结论、现状盘点、架构决策、里程碑、风险
- [API 设计](docs/02-API设计.md) —— 设计原则、各组件用法、如何接入新库
- [构建与交叉编译](docs/03-构建与交叉编译.md) —— 预设、工具链细节、踩坑记录、上板部署
- [推理方案与模型兼容性](docs/04-推理方案与模型兼容性.md) —— 模型转换、7 类形态实测、Zynq 算力边界、ORT 交叉编译
- [OpenCV 接入方案](docs/05-OpenCV接入方案.md) —— 各平台获取方式、armv7 裁剪编译、预处理整合
- [板端验证手册](docs/06-板端验证手册.md) —— 部署脚本、必跑项目、错误对照、ORT vs ncnn 判定
- [图像链路与无异常构建](docs/07-图像链路与无异常构建.md) —— 为什么用 stb、letterbox 精度、
  `-fno-exceptions` 的真值表与 CI 守护方式
- [信号处理与解调](docs/08-信号处理与解调.md) —— 频谱/滤波/检测/识别的判决树、
  解调实现要点、为什么暂不引 liquid-dsp、板端算力参考
- [机器学习模型集成](docs/09-机器学习模型集成.md) —— sklearn→ONNX 导出链路、my::ml API、
  golden 对拍、模型覆盖面与板端体积/延迟
- [网络与 HTTP](docs/10-网络与HTTP.md) —— 为什么 TCP/UDP 自己封、为什么 HTTP 引 cpp-httplib、  板端线程池/超时注意事项
- [基础组件（io / time / thread / 配置）](docs/11-基础组件.md) ——
  原子写为什么必要、测耗时为什么只能用单调时钟、线程池/周期任务的契约、配置分层合并
- [Int8 量化实测](docs/12-量化实测.md) —— 我们自己的工具测出来的 1.36~2.03×、
  为什么小模型反而更慢、为什么合成校准集不可信、以及 ARMv7 上别照搬 x86 数字
- [串口通信](docs/13-串口通信.md) —— 为什么自己封 COM/termios、超时为什么不算错误、PTY 上怎么真测、板端 dialout/by-id/流控实践
- [协议编解码与崩溃捕获](docs/14-协议编解码与崩溃捕获.md) —— CRC16/CRC32、hex/base64、COBS 分帧、`my::crash` 的现场输出与能力边界
- [测试用例数](docs/test-counts.md) —— 由 `mylib_tests --list` 生成的单一来源；CI 校验，避免手写数字漂移

## 三方库来源

`resources/` 里的压缩包是**构建期唯一真源**：首次 `cmake` 会自动解压到 `third_party/`，
之后完全离线可复现。但这些 zip 体积大、不进 git，所以 clone 之后要先补一次：

```powershell
.\scripts\fetch-deps.ps1     # 或 bash scripts/fetch-deps.sh
```

必需（缺了直接 FATAL_ERROR）：

| 包 | 版本 | 形态 |
|---|---|---|
| eigen-5.0.0.zip | 5.0.0 | 纯头文件 |
| json-include.zip | nlohmann/json 3.12.0 | 纯头文件 |
| spdlog-1.17.0.zip | 1.17.0 | 源码 + CMake |
| stb-image-2.32.zip | stb_image 2.32 / stb_image_write 1.18 | 两个 `.h`（按 commit pin + SHA256，由 fetch 脚本生成） |
| pocketfft-4d865cb.zip | pocketfft @ 4d865cb | 单个 `.h`（同上） |
| httplib-0.66.0.zip | cpp-httplib 0.66.0 | 单个 `.h` + LICENSE（同上） |

按平台自备（**不在** resources/ 里，缺了自动降级，不会阻断构建）：

| 目录 | 说明 | 缺了会怎样 |
|---|---|---|
| `onnxruntime-win-x64-1.23.2/` | Windows x64 预编译（MSVC ABI） | infer 回退 stub 后端 |
| `onnxruntime-armv7-petalinux2020/` | 板端 ORT 1.18.1（GCC 9.2 能编的最后一版） | armv7 回退 stub |
| `onnxruntime-linux-armv7-1.23.2/` | WSL + armhf 交叉编译，见 `scripts/build-ort-armv7-wsl.sh` | — |
| `ncnn-20250503/` / `ncnn-linux-armv7-20250503/` | ncnn 备选后端 | 只有 ORT，转不了 ncnn 的模型就没辙 |
| `opencv-4.12.0/` | OpenCV 官方 Windows 包（仅 MSVC 可用） | `cv` 组件消失 |

> armv7 上 `third_party` 里同时放着 1.18.1 和 1.23.2 两份 ORT 时会**优先选 1.18.1**
> （目录名排序 + 架构校验），这正是板子需要的版本。configure 摘要里会打印最终选中的包名。

## 许可证

本项目使用 **MIT License**，见 [LICENSE](LICENSE)。

随源码/二进制分发的三方库各自遵循其原始许可证：

| 依赖 | 许可证 |
|---|---|
| spdlog | MIT |
| nlohmann/json | MIT |
| cpp-httplib | MIT |
| stb_image / stb_image_write | MIT（或公有领域，二选一） |
| Eigen | MPL-2.0 |
| pocketfft | BSD-3-Clause |
| ncnn | BSD-3-Clause |
| ONNX Runtime | MIT |
| OpenCV | Apache-2.0 |

> `dist/` 里的 SDK 包内含 ONNX Runtime / OpenCV 的二进制，再分发时请一并保留它们的
> 许可证与 NOTICE（ORT 为 MIT；OpenCV 为 Apache-2.0）。
