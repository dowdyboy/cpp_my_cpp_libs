# my_cpp_libs

跨平台 C++17 三方库统一封装层 + 调用演示工程。

一套源码，在 **Windows(x64/MSVC、MinGW)** 与 **Linux(x86_64、armv7-A/Zynq-7000、aarch64)** 上
用同一份 CMake 配置完成编译，业务代码不需要写平台分支。

```
业务代码  ->  my_cpp_libs::{log, json, math, infer}  ->  spdlog / nlohmann::json / Eigen / ONNX Runtime
```

## 这是什么

1. **二次封装库**：把 `resources/` 里收集的开源库收进统一 API，错误处理统一成 `my::Status` / `my::Result<T>`，
   换库、换平台时业务代码不动。
2. **调用演示工程**：`examples/` 里既有封装层的用法，也有各三方库**原生 API** 的常用写法对照，
   可以直接当速查手册用。

## 快速开始

```powershell
# Windows / MinGW（最快）
cmake --preset win-mingw
cmake --build --preset win-mingw
ctest --preset win-mingw

# Windows / MSVC（唯一能用 ONNX Runtime 预编译包的方式）
cmake --preset win-msvc
cmake --build --preset win-msvc
ctest --preset win-msvc

# 交叉编译到 Zynq-7000（armv7-A 硬浮点）
cmake --preset linux-armv7
cmake --build --preset linux-armv7

# 五个平台一把梭
.\scripts\build-all.ps1

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
| core | `my_cpp_libs::core` | — | 全平台 | `Status` / `Result<T>` / 构建信息 / fp16 转换 |
| log | `my_cpp_libs::log` | spdlog 1.17 | 全平台 | 分级、异步、滚动、自定义 sink、未初始化兜底 |
| json | `my_cpp_libs::json` | nlohmann/json 3.12 | 全平台 | 路径式访问 `a.b[0].c`、异常转 Status、结构体映射 |
| math | `my_cpp_libs::math` | Eigen 5.0 | 全平台 | 姿态转换、线性方程、最小二乘、统计/滤波 |
| infer | `my_cpp_libs::infer` | ONNX Runtime 1.23 / ncnn | **x64 + armv7 + aarch64** | 双后端可插拔；含预处理/后处理（归一化、letterbox、softmax、NMS） |
| cv | `my_cpp_libs::cv` | OpenCV 4.12（可选） | x64（armv7 待编） | 图像 IO、letterbox 预处理、坐标逆变换 |

## 板端实测（Zynq-7020, Cortex-A9 @666MHz）

| 项目 | 结果 |
|---|---|
| 单元测试 | 63 / 63 通过（ORT + ncnn 双后端） |
| small_cnn（32×32） | 2.47 ms |
| bench_cnn（224×224, ≈300 MMAC） | **585 ms**（ncnn）/ **865 ms**（ORT） |
| MobileNetV2-12（官方模型） | **1367 ms**（ORT） |
| 内存峰值 | 140~153 MB |
| 数值一致性 | 与 x86 一致 |

> 板子是 PetaLinux 2020.2（glibc 2.30 / GCC 9.2），用配套 SDK 交叉编译。
> ORT 需要 1.18.1（1.19+ 要求 GCC 11），已经编好放在
> `third_party/onnxruntime-armv7-petalinux2020`。详见 [板端验证手册](docs/06-板端验证手册.md)。

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

# 板端部署（板子到手后）
.\scripts\deploy-and-bench.ps1 -Board <ip> -User root
.\scripts\deploy-and-bench.ps1 -Package     # 板子还没到：先打离线包
# ASan/UBSan 检查（WSL）
wsl -d Ubuntu-24.04 -u root -- bash scripts/run-tests-asan-wsl.sh
```

`build/<preset>/bin/model_inspect`，见 [推理方案](docs/04-推理方案与模型兼容性.md)。

## 目录结构

```
CMakeLists.txt          顶层：选项、平台探测、安装
CMakePresets.json       五个目标平台的预设
cmake/
  Platform.cmake        架构/平台宏、统一 target 工厂
  Deps.cmake            三方库挂载（自动解压 resources/*.zip）
  toolchains/           交叉编译工具链文件
include/my_cpp_libs/    对外公共头
src/                    组件实现
examples/               封装层示例 + 原生 API 示例 + 独立消费方工程
tools/                  命令行工具（model_inspect）+ 模型生成脚本
tests/                  无依赖的单元测试（69 个用例；MinGW 无 OpenCV 时 63 个）
models/                 测试模型（脚本生成，含 7 种形态）
scripts/                构建脚本、ONNX Runtime 交叉编译脚本
third_party/            由 resources/*.zip 自动解压（不入库）
docs/                   规划、API、构建、推理、OpenCV
```

## 文档

- [规划与可行性](docs/01-规划与可行性.md) —— 结论、现状盘点、架构决策、里程碑、风险
- [API 设计](docs/02-API设计.md) —— 设计原则、各组件用法、如何接入新库
- [构建与交叉编译](docs/03-构建与交叉编译.md) —— 预设、工具链细节、踩坑记录、上板部署
- [推理方案与模型兼容性](docs/04-推理方案与模型兼容性.md) —— 模型转换、7 类形态实测、Zynq 算力边界、ORT 交叉编译
- [OpenCV 接入方案](docs/05-OpenCV接入方案.md) —— 各平台获取方式、armv7 裁剪编译、预处理整合
- [板端验证手册](docs/06-板端验证手册.md) —— 部署脚本、必跑项目、错误对照、ORT vs ncnn 判定

## 三方库来源

`resources/` 里的压缩包是唯一真源，首次 `cmake` 时自动解压到 `third_party/`，完全离线可复现：

| 包 | 版本 | 形态 |
|---|---|---|
| eigen-5.0.0.zip | 5.0.0 | 纯头文件 |
| json-include.zip | nlohmann/json 3.12.0 | 纯头文件 |
| spdlog-1.17.0.zip | 1.17.0 | 源码 + CMake |
| onnxruntime-win-x64-1.23.2.zip | 1.23.2 | Windows x64 预编译（MSVC ABI） |
| onnxruntime-linux-armv7-1.23.2 | 1.23.2 | **本机交叉编译**（WSL + armhf 工具链），见 `scripts/build-ort-armv7-wsl.sh` |

另外两个由脚本落到 `third_party/` 的依赖（不进 git）：

| 目录 | 说明 |
|---|---|
| `ncnn-20250503` / `ncnn-linux-armv7-20250503` | ncnn 备选后端，MSVC 与 armv7 各编一份 |
| `opencv-4.12.0` | OpenCV 官方 Windows 包（仅 MSVC 可用） |
