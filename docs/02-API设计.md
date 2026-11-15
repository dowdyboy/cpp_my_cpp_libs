# API 设计

## 0. 总原则

1. **对外不用异常**：失败一律 `Status` / `Result<T>`。
2. **零依赖头尽量零依赖**：`log.hpp` 不引入 spdlog；`infer.hpp` 不引入 ORT。
3. **逃生舱**：封装挡不住的高级用法必须能下去（`raw()`、`write()`）。
4. **命名**：`my::<component>::<Thing>`，函数用小写下划线，类型用大驼峰。
5. **C++17**：不用概念、不用 `std::expected`、不用三路比较。

---

## 1. core —— 错误模型

```cpp
#include <my_cpp_libs/core/status.hpp>
```

### Status

```cpp
my::Status ok;                                    // 成功
my::Status err(my::Errc::IoError, "open failed"); // 失败 + 文本

ok.ok();            // true
err.code();         // my::Errc::IoError
err.message();      // "open failed"
err.to_string();    // "io_error: open failed"
static_cast<bool>(err);  // false
```

### Result\<T\>

```cpp
my::Result<int> r = 42;          // 成功
my::Result<int> e(my::Status(my::Errc::NotFound, "x"));

if (!r) { /* r.status() */ }
int v = r.value();               // 前置条件：r.ok()，否则 abort
int d = r.value_or(7);
auto m = r.map([](int x) { return x * 2; });   // 失败会原样传递
```

| Errc | 场景 |
|---|---|
| `InvalidArgument` | 参数非法、维度不匹配、路径语法错误 |
| `NotFound` | 文件/键/路径不存在 |
| `IoError` | 打开/读写文件失败 |
| `ParseError` | JSON 解析失败、类型转换失败 |
| `Unsupported` | 平台不支持（如 armv7 上要 onnxruntime） |
| `NotInitialized` | 需要先 init |
| `RuntimeError` | 运行期失败（模型加载失败、矩阵奇异） |

> `value()` 在失败状态被调用时会先往 stderr 打一行
> `[my_cpp_libs] Result<T>::value() 在失败状态被调用: <code>: <message>`，然后 `abort()`。
> 嵌入式上"莫名重启"至少能留下一条线索；测试里请用 `CHECK_OK_VALUE` 而不是裸 `.value()`。

### BuildInfo

```cpp
my::BuildInfo::summary();
// "my_cpp_libs 0.1.0 | arm-linux-gnueabihf | gcc 13.3.0 | Release | cross | @33817a257a73 | ptr=32"
my::BuildInfo::gitCommit();   // "33817a257a73"（工作区有未提交改动时带 "-dirty"）
```

上板第一件事就该打这一行——确认跑的是**哪一次提交**的产物，而不只是版本号。

---

## 2. log —— 日志

```cpp
#include <my_cpp_libs/log/log.hpp>    // 零依赖：init / write / sink
#include <my_cpp_libs/log/format.hpp> // 需要 {} 格式化时再包含
```

### 初始化

```cpp
my::log::Config cfg;
cfg.level        = my::log::Level::Debug;
cfg.to_console   = true;
cfg.use_color    = true;
cfg.file_path    = "logs/app.log";
cfg.rotate_bytes = 10 * 1024 * 1024;
cfg.rotate_files = 3;
cfg.async        = true;          // 高吞吐场景
cfg.async_queue_size = 8192;

auto st = my::log::init(cfg);
if (!st) { /* st.to_string() */ }
```

### 写日志

```cpp
my::log::debug("x={}, name={}", 42, "imu");
my::log::info("启动完成");
my::log::warn("队列 {} 超阈值", n);
my::log::error("设备无响应");
my::log::critical("退出");

// 参数昂贵时先判断
if (my::log::should_log(my::log::Level::Debug)) {
    my::log::debug("dump={}", dump_everything());
}
```

### 运行期控制

```cpp
my::log::set_level(my::log::Level::Warn);
my::log::level();
```

### 自定义出口（嵌入式重点）

```cpp
my::log::set_sink([](my::log::Level lv, std::string_view msg) {
    uart_send(my::log::level_name(lv), msg);   // Zynq 串口
});
```

设置后，日志在原有输出之外**额外**转发到这里。

### 特性

| 行为 | 说明 |
|---|---|
| 未 `init()` 也能用 | 落到 stderr，不丢日志、不崩 |
| `init()` 可重复调用 | 先 shutdown 再重建 |
| `shutdown()` 后写 | 回到 stderr 兜底 |
| 异步日志 | 退出前 `flush()` 或 `shutdown()`，否则可能丢尾部 |
| 阈值 | `write()` 先判 `should_log()`，sink 只会收到该收的 |

### 与原生 spdlog 的对照

| 需求 | 原生 | 封装 |
|---|---|---|
| 控制台 + 滚动文件 | 手工拼 `sink_ptr` 列表 | `Config.file_path` 一个字段 |
| 异步 | `init_thread_pool` + `async_logger` | `cfg.async = true` |
| 导到串口/GUI | 自己写 sink 类（要继承基类） | `set_sink(λ)` |
| 未初始化时 | 抛异常/无输出 | 落 stderr |

原生写法见 `examples/10_native_spdlog/main.cpp`。

---

## 3. json —— 配置/数据

```cpp
#include <my_cpp_libs/json/json.hpp>
```

### 路径语法

```
"a"             -> 根下 a
"a.b.c"         -> 嵌套对象
"arr[0]"        -> 数组元素
"a.b[2].c"      -> 混合
""              -> 根
```

### 读写

```cpp
my::json::Document doc;

doc.set("app.name", "demo");        // 自动创建中间对象
doc.set("devices[0]", "imu0");      // 自动创建数组、补 null
doc.set("server.port", 9000);

auto port = doc.get<int>("server.port");   // Result<int>
if (!port) { /* port.status().to_string() */ }

int  p = doc.get_or<int>("server.port", 8080);
bool has = doc.contains("server.port");
auto t   = doc.type_of("server.port");     // Type::Integer

doc.erase("server.port");
auto keys = doc.keys("server");
```

### 文件

```cpp
auto doc = my::json::Document::parse_file("config.json");
if (!doc) { /* IoError 或 ParseError */ }

doc.value().save_file("config.json", /*indent=*/2);
```

### 结构体映射（沿用 nlohmann 的宏）

```cpp
struct Camera { std::string name; int width; int height; };
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Camera, name, width, height)

Camera cam = doc.get<Camera>("camera").value_or(Camera{});
```

### 与原生 nlohmann 的对照

| 需求 | 原生 | 封装 |
|---|---|---|
| 取深层值 | `j["a"]["b"].get<int>()`，路径不存在会**抛异常**或插入 null | `doc.get<int>("a.b")` 返回 `NotFound` |
| 类型不匹配 | 抛 `type_error` | 返回 `ParseError` + 原因 |
| 造嵌套结构 | 逐层 `j["a"]["b"] = ...` | `doc.set("a.b", ...)` |
| 判存在 | `contains` + 手工逐层 | `doc.contains("a.b.c")` |

原生写法见 `examples/11_native_nlohmann/main.cpp`。

---

## 4. math —— 数学/姿态

```cpp
#include <my_cpp_libs/math/math.hpp>
```

> 这个头会引入 Eigen，编译时间会明显增加；不需要时别包含。

### 类型别名

```cpp
my::math::Vec2d Vec3d Vec4d VecXd Vec2f Vec3f
my::math::Mat2d Mat3d Mat4d MatXd
my::math::Quatd AngleAxisd
my::math::AlignedVector<T>       // = std::vector<T, Eigen::aligned_allocator<T>>
```

### 姿态约定（**全库统一，务必记住**）

```
R = Rz(yaw) * Ry(pitch) * Rx(roll)     // 内旋 ZYX / 外旋 XYZ
roll  ∈ [-π, π]
pitch ∈ [-π/2, π/2]
yaw   ∈ [-π, π]
单位：弧度
```

```cpp
using namespace my::math;

Mat3d R = rotation_zyx(deg2rad(10), deg2rad(-20), deg2rad(30));
Vec3d e = euler_zyx(R);                 // 反解，自动处理万向锁
Quatd q = quaternion_zyx(0.1, 0.2, 0.3);
Vec3d e2 = euler_zyx(q);

Mat4d T = make_transform(R, Vec3d(1, 2, 3));
auto  R2 = transform_rotation(T);       // Result<Mat3d>，会做正交性校验
Vec3d t  = transform_translation(T);
```

### 线性代数

```cpp
auto x = solve(A, b);            // Result<VecXd>：非方阵/奇异/NaN 都会报错
auto c = least_squares(A, b);    // Result<VecXd>：秩亏会报错
double k = condition_number(A);
```

### 统计

```cpp
std::vector<double> v = read_sensor();
mean(v); median(v); rms(v); standard_deviation(v);
auto smooth = moving_average(v, 15);
```

> `median(v)` 传值（内部拷贝），不改调用方数据；零拷贝用 `median_inplace(v)`。

### 与原生 Eigen 的对照

| 需求 | 原生 | 封装 |
|---|---|---|
| 欧拉角→矩阵 | 三个 `AngleAxisd` 连乘 | `rotation_zyx()`，且指定了旋转顺序 |
| 反解欧拉角 | `eulerAngles()` 顺序易错、万向锁返回 NaN | `euler_zyx()`，闭式解 + 万向锁分支 |
| 求解方程 | `A.partialPivLu().solve(b)`，奇异时返回垃圾值 | `solve()` 返回 `RuntimeError` |
| 定长类型进容器 | 必须记得 `aligned_allocator` | `AlignedVector<T>` |

原生写法见 `examples/12_native_eigen/main.cpp`。

---

## 5. infer —— 推理

```cpp
#include <my_cpp_libs/infer/infer.hpp>
```

### 基本用法

```cpp
my::infer::Options opt;
opt.model_path    = "model.onnx";
opt.intra_threads = 2;

auto session = my::infer::create_session(opt);
if (!session) {
    // armv7 上很可能就是这个分支：Unsupported
    my::log::error("{}", session.status().to_string());
    return;
}

for (const auto& p : session.value()->inputs())
    my::log::info("in {} {} {}", p.name, my::infer::dtype_name(p.type), p.shape.to_string());

auto out = session.value()->run_single_f32(input_data, {1, 3, 224, 224});
if (out) use(out.value());
```

### 后端

```cpp
my::infer::available_backends();   // {"stub", "onnxruntime", "ncnn"}
my::infer::has_backend("ncnn");
my::infer::build_backend();        // 编译期最强后端："onnxruntime" / "ncnn" / "stub"
```

自动挑选规则：

| 条件 | 选中的后端 |
|---|---|
| `opt.backend` 显式指定 | 就用它，不可用则报 `Unsupported` |
| 没指定 + 有 `model_path` | 按扩展名倾向：`.param` → ncnn 优先，其余 → onnxruntime 优先 |
| 没指定 + 没 `model_path` | `stub` |

> 有模型文件时**绝不会**退回 `stub`——那会让"模型跑通了"变成假象。
> 如果当前构建没有能加载模型的后端，`create_session` 直接返回 `Unsupported`。

### 后端数据模型

```cpp
struct Port  { std::string name; DType type; Shape shape; };   // 模型端口
struct Tensor{ DType type; Shape shape; void* data; size_t bytes; };  // 不拥有内存
struct Feed  { std::string_view name; Tensor tensor; };        // 输入
struct Outcome { std::string name; Tensor tensor; };           // 输出（Session 拥有）
```

**生命周期约定**：`Outcome::tensor.data` 的有效期到下一次 `run()`。
需要留存就自己拷走——这条约定和 ORT 原生行为一致，不要指望封装帮你拷贝。

### 加一个新后端（以 MNN 为例）

现有后端：`stub` / `onnxruntime` / `ncnn`。加新后端的完整流程：

```cpp
// src/infer/backend_mnn.cpp
#if defined(MYLIB_HAVE_MNN)
#include <MNN/Interpreter.hpp>

class MnnSession final : public my::infer::Session { /* ... */ };

void my::infer::detail::register_mnn_backend() {
    register_backend("mnn", [](const Options& o) -> Result<std::unique_ptr<Session>> {
        return std::unique_ptr<Session>(new MnnSession(o));
    });
}
#endif
```

然后：
1. 在 `src/infer/backends.hpp` 加声明；
2. 在 `src/infer/infer.cpp` 的 `ensure_builtin_backends()` 里加一行；
3. 在 `src/CMakeLists.txt` 里按需把源文件加进 `_infer_sources`；
4. 在 `cmake/Deps.cmake` 里加依赖探测（照抄 ncnn 那段，注意带架构校验）。

业务代码**一行都不用改**。

### 预处理 / 后处理

`#include <my_cpp_libs/infer/preprocess.hpp>` —— 纯数组运算，**不依赖 OpenCV**。
这些是"模型跑通但结果全错"的高发区，集中实现并测试。

```cpp
// 图像（BGR，u8）-> 张量（RGB，f32，NCHW）
my::infer::NormalizeSpec spec;
spec.src_order = my::infer::ColorOrder::BGR;   // cv::imread 的默认
spec.dst_order = my::infer::ColorOrder::RGB;   // 训练脚本的默认
spec.layout    = my::infer::Layout::NCHW;
spec.scale     = 1.0f / 255.0f;
spec.mean      = {0.485f, 0.456f, 0.406f};     // ImageNet
spec.stddev    = {0.229f, 0.224f, 0.225f};

std::vector<float> tensor;
my::infer::image_to_tensor(img, h, w, 3, spec, tensor);
```

| 函数 | 用途 |
|---|---|
| `image_to_tensor` | u8 HWC → f32 NCHW/NHWC，含颜色顺序与归一化；4 通道输入会**丢弃 alpha**、输出 3 通道 |
| `hwc_to_chw` / `normalize` | 已转 float 时的单独步骤；`normalize(src, pixels, channels, spec, out)` 按 `spec.layout` 取通道参数 |
| `letterbox_params` / `unletterbox_box` | 等比缩放 + 居中填充，以及把框映射回原图；`Letterbox` 带 `scale_x/scale_y`，**拉伸模式也还原得回来** |
| `softmax` / `argmax` / `topk` | 分类后处理 |
| `topk_softmax` | 一步拿到 top-k 概率 |
| `iou` / `nms` | 检测后处理（按类别抑制） |

> **参数必须和训练侧一致**。`scale/mean/stddev/颜色顺序/布局` 这五项，
> 任何一项错了都不会报错，只会输出一堆看起来正常的垃圾值。

---

## 6. cv —— 图像（OpenCV，可选组件）

```cpp
#include <my_cpp_libs/cv/image.hpp>
```

> 需要 `-DMYLIB_ENABLE_CV=ON` 且能找到 OpenCV。没找到时该组件整体消失，不影响其他组件。

### 定位：只做"OpenCV 到推理"这一层

`::cv::Mat`、`::cv::resize`、`::cv::cvtColor` 直接用，**不重复包装**。
封装只解决三件事：

1. 文件 IO 从"返回空 Mat"变成带原因的 `Result`；
2. `prepare()` 一次完成 resize → letterbox → 颜色 → 归一化 → HWC→CHW；
3. `LetterboxInfo` 跟着张量返回，后处理才能把框还原。

### 用法

```cpp
auto img = my::cv::load("test.jpg");
if (!img) { my::log::error("{}", img.status().to_string()); return; }

my::cv::PreprocessSpec spec;
spec.input_width  = 640;
spec.input_height = 640;
spec.keep_aspect  = true;                      // letterbox
spec.norm.src_order = my::infer::ColorOrder::BGR;   // imread 是 BGR
spec.norm.dst_order = my::infer::ColorOrder::RGB;
spec.norm.layout    = my::infer::Layout::NCHW;
spec.norm.scale     = 1.0f / 255.0f;
spec.norm.mean      = {0.485f, 0.456f, 0.406f};
spec.norm.stddev    = {0.229f, 0.224f, 0.225f};

auto prep = my::cv::prepare(img.value(), spec);
if (!prep) return;

auto out = session->run_single_f32(prep.value().tensor, prep.value().shape);
// prep.value().letterbox 用来把框映射回原图
```

| API | 说明 |
|---|---|
| `load / save` | 带错误原因的读写 |
| `prepare(img, spec)` | 一步到位地生成张量 + letterbox 参数 |
| `prepare_file(path, spec)` | 读文件 + prepare |
| `unletterbox_boxes(boxes, n, lb)` | 把模型坐标系下的框批量映射回原图 |

### 命名空间坑（重要）

组件名叫 `my::cv`，OpenCV 是全局 `cv`。在 `namespace my::cv {}` 里裸写 `cv::Mat`
会被解析成 `my::cv::Mat`。所以实现里**一律写 `::cv::Mat`**，
并且不能用 `CV_Assert`（它展开成 `cv::error(...)`，同样解析失败）。

---

## 7. 如何接入一个新三方库（五步）

以加一个 "yaml" 组件为例：

**① 放包**：`resources/yaml-cpp-0.8.0.zip`。

**② 挂载**：`cmake/Deps.cmake` 里加一段

```cmake
if(MYLIB_ENABLE_YAML)
    mylib_require_dep(yaml-cpp-0.8.0 "yaml-cpp-*.zip")
    add_subdirectory("${MYLIB_DEP_yaml-cpp-0.8.0}" "${CMAKE_BINARY_DIR}/_deps/yaml-cpp" EXCLUDE_FROM_ALL)
    add_library(deps_yaml INTERFACE)
    target_link_libraries(deps_yaml INTERFACE yaml-cpp::yaml-cpp)
    add_library(deps::yaml ALIAS deps_yaml)
endif()
```

**③ 公共头**：`include/my_cpp_libs/yaml/yaml.hpp`，不引入 yaml-cpp 的符号。

**④ 实现**：`src/yaml/yaml.cpp`，在边界把 `YAML::Exception` 翻成 `my::Status`。

**⑤ 注册**：`src/CMakeLists.txt` 加

```cmake
if(MYLIB_ENABLE_YAML)
    mylib_add_component(yaml SOURCES yaml/yaml.cpp LINK mylib_core deps::yaml)
    list(APPEND MYLIB_TARGETS mylib_yaml)
endif()
```

再加：一个封装层示例、一个原生对照示例、一组单元测试。

---

## 8. 常见陷阱清单

| 陷阱 | 后果 | 规避 |
|---|---|---|
| `std::vector<Eigen::Vector4d>` | ARM 上偶发 SIGBUS | 用 `my::math::AlignedVector` |
| 在 `INTERFACE` 编译定义里改三方库行为 | 污染所有消费方 | 自定义逻辑放 `src/` |
| 静态库里靠全局对象自注册 | 链接器丢弃 `.o`，注册"消失" | 显式 `register_xxx()` + `call_once` |
| 把 spdlog 的 `format_string` 当普通 `const char*` 传 | 编译期报错 | 格式化串必须是字面量 |
| 异步日志不 `flush` 就退出 | 尾部日志丢失 | 退出前 `flush()` 或 `shutdown()` |
| 让 `Result<T>::value()` 在失败时返回值 | 掩盖 bug | 已设计成 `abort()` |
| 用 `int` 当错误码跨模块传 | 无法携带上下文 | 统一用 `Status` |
| 在公共头里出现 `Ort::` 类型 | 无 ORT 的平台编不过 | 接口隔离，`Session` 是纯虚类 |
