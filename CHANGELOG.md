# 变更记录

## 0.1.1（2026-11-17）—— 缺陷修复 + 测试可信度 + CI 补全

这一版**没有新功能**，全部是"把已有的东西弄对"。起因是一次全量评估：构建和 158 个用例
在两个宿主工具链上全绿，但安全网本身有洞 —— 最典型的是"Debug 构建必崩、而 CI 里没有任何
Debug job"。（评估报告与验证脚本在本机 `assess/` 目录，含有本机路径，不入库。）

### 修复的缺陷（会崩 / 会算错 / 会静默错）

| 编号 | 位置 | 问题 | 修法 |
|---|---|---|---|
| A1 | `src/math/math.cpp:126` | `least_squares()` 把矩阵传给 `all_finite(Eigen::Ref<const VecXd>)` | 改用同文件的 `matrix_all_finite()`；Debug 构建下原来会触发 Eigen 断言 |
| A2 | `src/dsp/demod.cpp:56` | `offset > size` 时负数转 `size_t`（UB），随后 `reserve` 抛 `length_error` 穿透 `Result` | 加 `offset >= size` 防护，直接返回空 |
| A3 | `src/http/http.cpp:174` | URL 里出现 `]` 就跳过端口解析 → `http://[::1]:8080` 静默连到 80 端口；`stoi("8080abc")` 被接受 | 重写 host/port 解析：IPv6 先定位 `]`，端口用 `consumed` 全串校验 |
| A4 | `src/log/log.cpp:42` | `static State`（含 mutex）随静态析构销毁，与"任何阶段可调"的承诺冲突 | 改为永不析构的静态指针（有意泄漏一个对象） |
| A5 | `include/.../core/status.hpp:60` | `Status` 没有 `[[nodiscard]]`，导致启动失败被 `(void)` 静默丢弃 | 加 `[[nodiscard]]`；`Server` 的 6 个 void 路由重载失败时向 stderr 上报 |
| A6 | `include/.../json/json.hpp` | `set("a[1e9]", v)` 无上限 → 一次调用吃掉几 GB 内存 | 路径下标上限 `kMaxSparseIndex = 65536`；超长数字串在解析期就拒绝（防 `size_t` 回绕） |
| A7 | `src/infer/backend_onnxruntime.cpp:190` | 输出字节数按端口声明类型算，与模型实际类型不符时下游越界读 | 用 `GetElementType()` 比对，不一致返回 `Unsupported` |
| A8 | `src/image/image.cpp:249`、`src/net/net.cpp:498` | `resize` / `recv_exact` 只校验下界，`bad_alloc` 从 `Result` 边界穿出 | 输出上限 512 MiB / 256 MiB，超限返回 `OutOfRange` |
| A9 | `src/net/net.cpp:345` | `native_handle()` 把 Win64 的 `SOCKET` 截成 `int`；`valid()` 用 `fd_ >= 0` | 返回 `std::intptr_t`；`valid()` 与平台无效句柄比较 |
| A10 | `include/.../math/math.hpp:90`、`src/core/build_info.cpp:53` | `noexcept` 函数里构造带分配的 `Status`/`std::string` → 分配失败即 `terminate` | `transform_rotation` 去掉 `noexcept`；`summary()` 用 try/catch 降级 |

### 顺带修掉的两处

- `src/http/http.cpp`：在 handler 里调 `Server::stop()` 会 join 自己（抛 `system_error`）；
  并发 `stop()` 会双重 join（UB）。改为"自身线程 detach + `thread_mutex` 保护"。
- `tests/test_framework.hpp`：`CHECK_NEAR` 用 `fabs(a-b) > eps` 判定，对 **NaN 恒为假**
  → 全仓 180 处浮点断言对 NaN 免疫。改为 NaN 安全的 `mytest::near()`。

### 补测试过程中又挖出来的（同样已修）

| 位置 | 发现方式 | 问题 | 修法 |
|---|---|---|---|
| `src/http/http.cpp` `Server::stop()` | 新增的 `http_stop_from_handler_is_safe` | **真死锁**：httplib 的 handler 跑在**线程池 worker** 上，判"当前线程==监听线程"根本不够；worker 里 join 监听线程，而监听线程退出要 join 全部 worker → 环 | handler 上下文用 `thread_local` 深度标记；该路径只置停止标志 + `detach`，且**不阻塞**在 mutex 上（用 `try_to_lock`，避免和外部 `stop()` 的 join 再次成环） |
| `include/.../json/json.hpp` `parse_path` | 新增的 `json_set_creates_nested_containers` | `"a.b[0].c"` —— 头文件注释里写着的语法 —— 实际返回 `InvalidArgument`（`']'` 后紧跟 `.` 被误判成"空键"） | 允许"刚解析完 `[N]`、没有 pending 键"的 `.`；同时拒绝 `a..b` / `a[0].[1]` |
| `src/net/net.cpp` `recv_exact` | 新增的 `net_recv_exact_rejects_oversized` | 先 `Bytes out(size)` 再查 `valid()`：未连接的 socket + 256 MiB 请求会先吃掉 256 MiB 才报错 | 顺序改为"参数校验 → 有效性 → 分配" |
| `tests/test_preprocess.cpp` | **ASan**（修好 A1 之后第一次真正跑到这里） | 新用例自己给了 3 个 float 却按 pixels*channels=6 读 → 栈越界（是测试的问题，不是库的） | 缓冲区给足 6 个；`normalize` 的"调用方保证 pixels*channels"契约在用例里写清 |

> 平台差异（未改语义，只写进文档）：Windows 的 `SO_REUSEADDR` 允许第二个 socket 绑同一端口，
> 与 POSIX 语义不同 —— 端口冲突检测在 Windows 默认失效，需要 `reuse_addr=false`。
> 详见 [docs/10 §6](docs/10-网络与HTTP.md)。

### 最终验证矩阵（本次改动后的实测）

| 配置 | 结果 |
|---|---|
| MinGW x64 Release（无 ORT/OpenCV） | **201/201** 通过，199 用例 + 2 个守卫；8 个 ml 用例显式 `[SKIP]` |
| MSVC x64 Release（ORT + ncnn + OpenCV 4.12） | **209/209** 通过；该构建自动带 `--fail-on-skip`，ml 必须真跑 |
| Linux gcc **Debug**（断言全开） | **201/201** 通过 ← A1 的守门配置 |
| Linux ASan（Debug + 泄漏检测） | 199 用例 0 失败（8 skip） |
| Linux TSan | 201/201，无数据竞争报告 |
| `-fno-exceptions` 子集 | **90/90** 通过 |
| 最小配置（只留 core） | 只产出 1 个静态库 |
| `cmake --install` → 外部工程 `find_package` | 编译 + 运行通过，Config 传播 `cxx_std_17` |


### 测试

- 新增 `tests/test_framework_self.cpp`：框架自测（NaN / ±inf / 容差边界 / 注册表）。
- 新增 `MYTEST_SKIP("原因")`：跳过会显式打印 `[SKIP]` 并计入汇总；配合 `--fail-on-skip`
  可以让"该跑却没跑"变成失败。有真 ORT 后端的构建会自动给每个用例传它，
  于是 ml 的 golden 对拍不会再有"静默空转还显示 OK"。
- 新增 `--expect-cases=N` 用例数守卫：CTest 注册表与二进制实际注册的用例数不一致就失败。
- 新增并发、错误注入、边界值与回归用例（net / log / dsp / json / infer / preprocess /
  http / math / core / image / ml），并修掉 2 条恒真断言。
- 崩溃兜底：测试进程现在会装 SIGSEGV/SIGABRT/SIGFPE 处理器，崩溃时打印是哪个用例崩的。

### CI

新增 3 个 job（`.github/workflows/ci.yml`）：

- `linux-debug`：**唯一**构建 Debug 的 job。Release 家族都会定义 `NDEBUG`，
  Eigen/STL 断言与前置条件检查一次都不会执行 —— A1 就是被这一点藏住的。
- `linux-sdk-consumer`：`cmake --install` → 外部工程 `find_package(my_cpp_libs CONFIG)`
  → 编译并**运行**（`tests/sdk_consumer/`）。同时断言 SDK 的 Config 传播了 `cxx_std_17`。
- `linux-tsan`：`MYLIB_SANITIZE=thread`，守住 log/infer/http 声明了线程安全却零并发测试的空档。

同时把 `linux-asan` 的构建类型从 `RelWithDebInfo` 改成 `Debug`（与
`scripts/run-tests-asan-wsl.sh` 对齐），否则断言那半边永远是关着的。

### 构建 / 分发

- `cmake/my_cpp_libsConfig.cmake.in`：补 `INTERFACE_COMPILE_FEATURES cxx_std_17`。
  以前只有 `add_subdirectory` 用法会传播 C++17 要求，SDK 用法的下游工程若默认 C++14 会编不过。
- `MYLIB_ENABLE_CORE=OFF` 改成 fail-closed（core 是所有组件的地基，原来是个空开关）。
- 配置摘要里的 `Components` 改为按**实际建出的 target** 打印，不再打印选项值
  （原来"选项开着但组件被跳过"时摘要仍显示 ON）。
- `MYLIB_SANITIZE` 新增 `thread`。
- `tools/model_inspect`：预热失败不再被忽略。

### 已知限制（本次未改）

- `dsp::find_peaks()` 在"单音 + 平坦噪声底"的谱上会返回若干贴着噪声底的假峰
  （需调 `min_prominence_db`）。是否改成"真实 prominence"或"按强度排序"需要单独决策。
- `dist/` 下的 4 个 SDK 包仍是 0.1.0 的产物，且不在 git 里 —— 本版改动后需要重新生成。
- 文档里"板端实测"一节是 0.1.0 的历史数据，未在 0.1.1 上重测。

## 0.1.0

首个可用版本：core / log / json / math / infer / preprocess / image / dsp / ml / net / http / cv
12 个组件，5 个目标平台，158 个用例，可 `find_package` 的 SDK。
