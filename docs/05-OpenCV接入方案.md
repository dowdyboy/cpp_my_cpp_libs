# OpenCV 接入方案

> **更新（2026-11）**：板端"解码 + 缩放 + letterbox"这一段已经由
> [`my::image`](07-图像链路与无异常构建.md)（stb，两个头文件）覆盖，
> **不再需要为了读图/缩放去交叉编译 OpenCV**。
> 现在这份文档解决的是另一类问题：上位机（x64）的图像处理，
> 以及板上确实要用到 OpenCV 算法（形态学、轮廓、标定、DNN）的场景。
> 如果只是"相机 → 预处理 → 推理"，直接用 `my::image`，别编 OpenCV。

> 结论：**可行，但必须自己交叉编译**。
> Windows x86 有官方预编译包可以直接用；Zynq 上官方没有 armv7 包，需要用 PetaLinux SDK 自己编。
> 好在 OpenCV 支持 `BUILD_LIST` 裁剪，只留 `core/imgproc/imgcodecs` 时体积和编译时间都可控。

---

## 1. 为什么需要 OpenCV

它解决的不是"图像处理"这个泛泛的需求，而是**推理链路里最容易出错的那一段**：

| 环节 | 自己写的问题 | OpenCV 提供 |
|---|---|---|
| 读图（jpg/png） | 得引入 libjpeg/libpng + 写解码 | `cv::imread` 一行 |
| 缩放 | 插值算法与训练侧不一致（cv2 默认 bilinear，PIL 默认 bicubic） | `cv::resize` 显式指定插值 |
| 颜色空间 | BGR/RGB 搞反 → 结果全错但不报错 | `cv::cvtColor` |
| letterbox | 自己算 scale/padding，容易和训练侧差几个像素 | 手写十来行，但要和训练脚本对齐 |
| 画框/存图 | 调试时想看一眼检测结果 | `cv::rectangle` / `cv::imwrite` |

**核心价值在"和训练侧保持一致"**，而不是算法本身。

---

## 2. 各平台获取方式

| 平台 | 推荐方式 | 说明 |
|---|---|---|
| Windows x64 / MSVC | 官方预编译 `opencv-4.x-windows.exe` | 自带 `vc16/vc17` 的 `opencv_world4xx.lib`，开箱可用 |
| Windows x64 / MinGW | MSYS2 `mingw-w64-ucrt-x86_64-opencv` | 官方包只有 MSVC ABI，MinGW 不能混用 |
| Linux x86_64 | `apt install libopencv-dev` 或自编 | 版本随发行版 |
| **Linux armv7 / Zynq** | **用 PetaLinux SDK 交叉编译** | 官方无 armv7 包 |
| Linux aarch64 | 自编（交叉）或板端编译 | 同上 |

> 注意：OpenCV 是 C++ 库，**没有跨编译器的 ABI 兼容**。
> 这一点和本项目"源码分发"的原则一致——你的消费方工程用什么工具链，就用什么工具链编 OpenCV。

---

## 3. Zynq 上的交叉编译

> **已经跑通了**（2026-11-15，VMware 上的 PetaLinux 2020.2 SDK，VM 12 核）。
> 一键脚本：`scripts/build-opencv-armv7-petalinux2020.sh`（在 VM 里跑）。
>
> 实测结果：
>
> | 项 | 值 |
> |---|---|
> | 模块 | core + imgproc + imgcodecs |
> | 三个 `.so` 合计 | **5.9 MB**（core 2.1 / imgproc 2.5 / imgcodecs 1.1） |
> | 安装树（含头文件） | 21 MB |
> | 构建时间 | 约 3 分钟（12 核） |
> | 最高 GLIBC | **2.29**（板子 2.30，够） |
> | 最高 GLIBCXX | 3.4.26（板子 3.4.28，够） |
> | 整工程验证 | `linux-armv7-sdk2020` + `-DMYLIB_ENABLE_CV=ON -DMYLIB_ENABLE_NCNN=ON` 编译、链接通过；产物的 `NEEDED` 里有 `libopencv_core.so.412` 等 |
>
> 产物 `dist/opencv-4.12.0-armv7/` 拷到 Windows 的
> `third_party/opencv-4.12.0-armv7/` 即可，`Deps.cmake` 会自动发现并做
> **ELF 架构校验**（Windows 上会自动跳过这棵 ELF 树，不会误链）。
>
> ✅ 板端已实测（2026-11-16）：三个 `.so` 拷到 `/home/root/board_deploy/lib`，`example_opencv`
> 跑通（320 ms，含推理与 PNG 编码），`cv` 用例 8/8 通过。
>
> ✅ **aarch64（Pi 4B）也通了**：用同一份 4.12 源码交叉编（core/imgproc/imgcodecs，自带 png/jpeg，
> 共 9.4 MB），`cv` 8/8 通过，`example_opencv` 64 ms。注意交叉编要加
> `-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY`，且 `-no-pie` 不能给共享库（见 [03 §3.3](03-构建与交叉编译.md)）。
>
> ⚠ 板端运行时：OpenCV 是三个动态库，需要把 `lib/*.so*` 拷到板子并
> `export LD_LIBRARY_PATH=/opt/mylib:$PWD`（和 ORT 一样）。
> 静态链接不是默认选项：`BUILD_SHARED_LIBS=OFF` 也能编，但会让每个可执行文件
> 大 6 MB 左右。

### 3.1 关键参数

```cmake
cmake -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=<本项目>/cmake/toolchains/linux-armv7-zynq.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=<install> \
  # ---- 只留三个模块，其余全关 ----
  -DBUILD_LIST=core,imgproc,imgcodecs \
  -DBUILD_SHARED_LIBS=ON \
  # ---- 关掉一切非必需 ----
  -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF \
  -DBUILD_opencv_apps=OFF -DBUILD_JAVA=OFF -DBUILD_opencv_python3=OFF \
  -DWITH_IPP=OFF -DWITH_OPENCL=OFF -DWITH_ITT=OFF -DWITH_ADE=OFF \
  -DWITH_GTK=OFF -DWITH_QT=OFF -DWITH_V4L=OFF -DWITH_FFMPEG=OFF \
  # ---- 编解码器用 OpenCV 自带版本，避免额外交叉编译 ----
  -DWITH_JPEG=ON  -DBUILD_JPEG=ON \
  -DWITH_PNG=ON   -DBUILD_PNG=ON \
  -DWITH_ZLIB=ON  -DBUILD_ZLIB=ON \
  -DWITH_TIFF=OFF -DWITH_WEBP=OFF -DWITH_OPENEXR=OFF \
  -DOPENCV_GENERATE_PKGCONFIG=OFF \
  -DOPENCV_ENABLE_NONFREE=OFF
```

### 3.2 实测结果（2026-11-15）

| 配置 | 体积 | 编译时间 |
|---|---|---|
| 全模块（默认） | 未测（文档估 ~60 MB） | 40~60 min |
| **`core,imgproc,imgcodecs` + 动态库** | **5.9 MB**（三个 .so） | **≈3 min（12 核）** |

单条可执行文件只多出几百 KB（链接的是共享库），部署时把三个 `.so` 一起拷上板即可。
对 Zynq 的 512 MB DDR / 几十 MB rootfs 完全够用。

### 3.3 注意事项

1. **`BUILD_JPEG/PNG/ZLIB=ON`** 让 OpenCV 用自带的 libjpeg/libpng，避免再去交叉编译这三个依赖。
2. **不要开 `WITH_OPENCL`**：Cortex-A9 没有可用 GPU，开了只是增加配置复杂度。
3. **`imgcodecs` 依赖 `imgproc` 和 `core`**，`BUILD_LIST` 里三个都要有。
4. 交叉编译时 OpenCV 会尝试探测宿主机的库（比如 `libgtk`），`CMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY`
   已经在 toolchain 里设好了，一般不会误用宿主的库。

---

## 4. 与本项目的集成设计

### 4.1 组件边界

新增组件 `my_cpp_libs::cv`，**可选**（`MYLIB_ENABLE_CV=OFF` 时完全不编译）：

```
include/my_cpp_libs/cv/image.hpp      对外 API（会引入 OpenCV 头，和 Eigen 一样）
src/cv/image.cpp                      实现
```

### 4.2 API 草案

封装的取舍：**不重复 OpenCV 已有的东西**（`cv::Mat` 直接用，`cv::resize` 不去包一层），
只在"OpenCV 和推理之间"这一层做整合：

```cpp
namespace my::cv {

using Image = cv::Mat;

// 文件 IO：把"返回空 Mat"改成带错误原因的 Result
Result<Image> load(const std::string& path, int flags = cv::IMREAD_COLOR);
Status        save(const std::string& path, const Image& image);

// ---------------------------------------------------------------------------
//  推理预处理：这是封装的主要价值
// ---------------------------------------------------------------------------
struct PreprocessSpec {
    int            input_width  = 640;
    int            input_height = 640;
    bool           keep_aspect  = true;        // true = letterbox，false = 直接拉伸
    ColorOrder     src_order    = ColorOrder::BGR;   // cv::imread 默认 BGR
    ColorOrder     dst_order    = ColorOrder::RGB;   // 大多数训练用 RGB
    float          scale        = 1.0f / 255.0f;
    std::array<float,3> mean{0.0f, 0.0f, 0.0f};
    std::array<float,3> stddev{1.0f, 1.0f, 1.0f};
    bool           nchw         = true;        // false = NHWC
};

// letterbox 的几何参数，后处理要用它把框映射回原图
struct LetterboxInfo {
    float scale   = 1.0f;   // 等比缩放比例（stretch 模式下等于 scale_x，仅供参考）
    float scale_x = 1.0f;   // x 方向比例：拉伸模式下与 scale_y 不同，逆变换必须分别用
    float scale_y = 1.0f;
    int   pad_x   = 0;
    int   pad_y   = 0;
};

struct PreparedImage {
    Image              resized;     // 送给模型的图
    LetterboxInfo      letterbox;
    std::vector<float> tensor;      // NCHW/NHWC 排好的 float 数据
};

Result<PreparedImage> prepare(const Image& src, const PreprocessSpec& spec);
Result<PreparedImage> prepare_file(const std::string& path, const PreprocessSpec& spec);

// 把模型输出坐标映射回原图坐标
void unletterbox_boxes(float* boxes, std::size_t count, const LetterboxInfo& info,
                       int src_width, int src_height);

}  // namespace my::cv
```

设计要点：

- `prepare()` 一次完成 **resize → letterbox → 颜色转换 → 归一化 → HWC→CHW**，
  这些都是"错了不报错、只出错误结果"的环节，集中在一个函数里才不会各写各的；
- `LetterboxInfo` 必须跟着 `tensor` 一起返回，否则后处理没法把框还原到原图；
- `keep_aspect=false`（拉伸）时 x/y 两个方向的比例不同，`scale_x` / `scale_y` 必须分开记，
  只留一个 `scale` 会让框在 y 方向整体偏移（这是 F 期修掉的缺陷之一）；
- 底下调用 `my::infer` 的纯数组预处理函数（见 §5），OpenCV 只负责解码和缩放。

> 上面是设计草案，最终落地签名以 [02-API设计](02-API设计.md) §6 为准
> （例如归一化参数收进了 `NormalizeSpec`，`layout` 用枚举而不是 `nchw` 布尔）。

### 4.3 CMake 接入

```cmake
option(MYLIB_ENABLE_CV "构建 OpenCV 组件" OFF)

if(MYLIB_ENABLE_CV)
    # 优先用用户显式指定的路径，其次 find_package
    if(MYLIB_OPENCV_ROOT)
        set(OpenCV_DIR "${MYLIB_OPENCV_ROOT}/lib/cmake/opencv4")
    endif()
    find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
    mylib_add_component(cv
        SOURCES cv/image.cpp
        LINK mylib_core mylib_infer deps::opencv)
endif()
```

`MYLIB_OPENCV_ROOT` 走 cache 变量，这样同一个工程在 x86 和 Zynq 上只是传不同的路径，
和 `MYLIB_ONNXRUNTIME_ROOT` 是同一套思路。

---

## 5. 不依赖 OpenCV 的那一半（先做）

有一批预处理/后处理是纯数组运算，**完全可以不引入 OpenCV**，而且更值得先做：

| 功能 | 为什么重要 |
|---|---|
| `u8 HWC → f32 NCHW` + 归一化 + BGR↔RGB | 推理预处理的标准路径，OpenCV 只是提供"解码后的 buffer" |
| `softmax` / `argmax` / `top-k` | 分类后处理 |
| `letterbox` 参数计算与逆变换 | 检测后处理 |
| `nms`（非极大值抑制） | 检测后处理，且必须在 C++ 侧做（模型不输出解码结果） |

这部分可以**立刻实现并测试**，等 OpenCV 接进来时，`prepare()` 只是把"解码 + resize"
换成 OpenCV 调用，其余逻辑复用。

> 实际顺序建议：先做这一半（无依赖、可测），再接 OpenCV（需要交叉编译环境）。

---

## 6. 分阶段实施

| 阶段 | 内容 | 前置条件 | 状态 |
|---|---|---|---|
| **C0** | 纯数组预处理/后处理（§5） | 无 | ✅ **已完成**（`infer/preprocess.hpp`，25 个测试） |
| **C1** | Windows 接官方预编译 OpenCV，跑通 `prepare()` | 下载 opencv-4.x-windows.exe | ✅ **已完成**（见 §8 实测） |
| **C2** | Linux x86 接入（apt 或自编） | WSL 可用 | ✅ 已完成（armv7 交叉编译一并在 §3 完成） |
| **C3** | Zynq 交叉编译 OpenCV（裁剪版） | PetaLinux SDK | ✅ **已完成**（2026-11-15，3 个 .so 共 5.9 MB，板端 8/8 通过） |
| **C4** | 端到端：读图 → 预处理 → 推理 → 后处理 → 存图 | C1 + ORT | ✅ **C1 侧已跑通** |

---

## 7. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| armv7 交叉编译 OpenCV 失败 | 无法在板端读图/缩放 | 退路：只用 C0 的纯数组路径，图像解码放在上位机 |
| 二进制体积膨胀 | rootfs 压力 | `BUILD_LIST` 裁剪 + 静态链接 + `strip`；实测可控制在 5MB 内 |
| OpenCV 版本与训练侧不一致 | resize/颜色结果有细微差异 | 固定版本，写进文档；关键流程用数值测试锁死 |
| MSVC/MinGW ABI 混用 | 链接失败或运行崩溃 | 官方包只配 MSVC；MinGW 用 MSYS2 包，绝不混用 |
| `::cv::Mat` 与 `my::infer::Tensor` 生命周期混乱 | 悬垂指针 | `prepare()` 返回自己持有的 `std::vector<float>`，Tensor 指向它 |

---

## 8. C1 实测记录

### 8.1 接入方式

用官方包 `opencv-4.12.0-windows.exe`（178MB），解压后目录结构：

```
third_party/opencv-4.12.0/opencv/build/
├── OpenCVConfig.cmake          ← 有，但缺 OpenCVModules.cmake
└── x64/vc16/lib/
    ├── OpenCVConfig.cmake      ← 两个都在，这才是能用的 OpenCV_DIR
    ├── OpenCVModules.cmake
    ├── opencv_world4120.lib    (3.6 MB, 导入库)
    └── ../bin/opencv_world4120.dll   (61 MB)
```

CMake 侧要点（`cmake/Deps.cmake`）：

1. 自动在 `third_party/opencv-*/...` 里找**同时含** `OpenCVConfig.cmake` 和
   `OpenCVModules.cmake` 的目录；
2. **必须通过 `OpenCV_DIR` + config 模式**找包。用
   `find_package(OpenCV CONFIG PATHS <dir> NO_DEFAULT_PATH)` 会失败——这个坑踩过一次；
3. 找到后把 `opencv_world4120.dll` 自动拷到可执行文件旁边（`mylib_copy_runtime_dlls()`），
   注意排除 `...d.dll` 调试版（127MB，没必要拷）。

### 8.2 命名空间冲突（重要坑）

组件叫 `my::cv`，而 OpenCV 的命名空间是全局的 `cv`。在 `namespace my::cv {}` 内部，
裸写 `cv::Mat` 会被解析成 `my::cv::Mat`，编译直接报错。

两个解法：

- 全部写 `::cv::Mat`（本项目采用）；
- 或者加 `using namespace ::cv;`——但会和 `my::cv` 里的同名函数打架，不推荐。

还有个连带问题：`CV_Assert` 宏展开成 `cv::error(...)`，在 `my::cv` 里同样会解析失败，
所以代码里没用 `CV_Assert`，改成显式 `if (...) return Status(...)`。

### 8.3 跑通的东西

```
输入图像: 160x120, 3 通道
letterbox: scale=0.2000 pad=(0, 4)      ← 160x120 等比缩到 32x32，上下各补 4 像素
张量: 3072 个 float, 建议形状 [1, 3, 32, 32]
后端: onnxruntime
top-3: class #2 prob=0.1216 ...
输入图中心 (16,16) 映射回原图 = (80.0, 60.0)   ← 160x120 的正中心，逆变换正确
结果已保存: output/cv_result.png
```

产物 `output/cv_result.png`：160x120 的渐变图，左上角绿字 `class 2 (0.12)`，
中心一个红点（letterbox 逆变换标出的原图中心）。

### 8.4 测试

`tests/test_cv.cpp` 11 个用例：

| 用例 | 验证点 |
|---|---|
| `cv_save_and_load` | PNG 往返，像素值不变 |
| `cv_load_missing` | 读不存在的文件返回 `IoError` 而不是空 Mat |
| `cv_prepare_letterbox` | 160x120→32x32：scale=0.2、pad_y=4、NCHW 排布、填充区=114/255 |
| `cv_prepare_stretch` | 直接拉伸时无 padding |
| `cv_prepare_gray_input` | 灰度图自动转 3 通道 |
| `cv_prepare_gray_model` | 灰度模型（input_channels=1）的 1 通道输出 |
| `cv_prepare_rgba_order_follows_src_order` | 4 通道按 BGRA/RGBA 各自的 src_order 处理 |
| `cv_prepare_letterbox_uses_rounded_scale` | scale_x/scale_y 用取整后的实际比例，与 my::image 一致 |
| `cv_unletterbox_boxes` | 坐标逆变换数值正确 |
| `cv_utf8_path_roundtrip` | 中文路径 save/load（Windows imread/imwrite 的 ANSI 问题回归） |
| `cv_prepare_rejects_huge_canvas` | 超大画布返回 `OutOfRange`，不抛异常 |

### 8.5 下一步（C2/C3）

- **C2**：Linux x86。最省事是 WSL 里 `apt install libopencv-dev`，但版本（4.9）和 Windows 的 4.12 不一致，
  要做一次数值对齐验证（resize 的边界处理偶有差异）。
- **C3**：armv7 裁剪编译。实际用时约 3 分钟，产出动态库三个 .so 共 5.9 MB（板端已验证）。
  **建议等模型定下来再做**——现在编了可能白编。
