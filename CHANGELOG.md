# 变更记录

## 0.1.6（2026-11-20）—— serial 串口组件

第 16 个组件：Windows COM / Linux termios 的薄封装，无三方依赖。

### 1. 为什么自己写

平台差异只有几处（打开、配置、超时、可读字节数、清缓冲、枚举、独占），集中在
`src/serial/serial.cpp` 里；引 Boost.Asio 换来的是用不到的异步/定时器，在 Zynq 上
编译代价比整个库还大。

### 2. API 与两条关键契约

```cpp
my::serial::Config cfg;  cfg.baud = 921600;  cfg.data_bits = 8;
auto opened = my::serial::Port::open("/dev/ttyUSB0", cfg);
std::uint8_t buf[256];
auto n = opened.value().read_some(buf, sizeof buf, std::chrono::milliseconds(100));
```

1. **`read_some()` 超时返回 0 字节 + Ok，不是错误**。串口"暂时没数据"是常态，
   轮询循环不该用错误码做控制流。（`my::net` 的 `recv` 超时返回 `Errc::Timeout`，
   因为 TCP 超时通常意味着对端出问题 —— 两处头文件都写明了这个差异，别记混。）
2. **`write_some()` 超时返回已写字节数 + Ok**；要"必须写完"用 `write_all()`（写不完报
   `Errc::Timeout` 并在消息里带"已写 X/Y 字节"）。

其余：`read_line`（命令式协议）、`bytes_available`、`flush`、`list_ports`（Windows 读注册表
`SERIALCOMM`；Linux 扫 `/dev/tty*` + `/dev/serial/by-id/*`）、`native_handle()`（自己 poll）。

波特率：Windows 任意值；Linux 只支持 termios 标准档位（50 ~ 4 Mbaud），
给非标准值报 `InvalidArgument` 并列出支持范围 —— **不静默降级**。

### 3. 怎么测的（没有串口硬件）

| 层 | 用例数 | 平台 |
|---|---|---|
| 参数校验 / 枚举 / 移动语义 | 8 | 全平台 |
| 真实收发（**PTY 对**：master 当对端设备，slave 交给 `Port::open`） | 9 | Linux（Windows 上 skip） |

PTY 让 `read/write/poll/FIONREAD/tcflush` 走真实驱动路径而不是打桩。用例专盯两个
最容易出错的地方：**超时边界**与**部分写**（例如用 64 KB 灌爆 PTY 缓冲，验证
`write_all` 绝不永久阻塞）。CI 里这 9 条跑在 Linux 全组件 job 与两个 QEMU job（ARM 真跑）。

### 4. 顺带修掉的三个问题

1. **SDK 配置里的硬编码组件列表**：`find_package` 生成的 `my_cpp_libsConfig.cmake` 有两处
   手写组件列表（`_mylib_sdk_add_component` 与 `my_cpp_libs::all` 的链接），加 serial 时
   漏了 —— 结果"装完之后能 find_package、能编，链接时 serial 符号全 undefined"。
   这是**消费方测试**抓出来的（正好是 0.1.3 那次评估里 C1 担心的漂移）。
2. **`--fail-on-skip` 误伤**：该开关本意是"有 ORT 时 ml 不许静默跳过"，但 Windows MSVC job
   开着它，而 serial 的 9 条 PTY 用例在 Windows 上必然跳过 → 会把合法环境跳过判成失败。
   改成逐用例放行（`serial_*` 不带该开关）。
3. **测试框架 `CHECK_OK_VALUE` 绑定成 `const auto&`**：move-only 的 `Result<Port>` 用不了，
   且 const 引用让 `port.read_some()` 这类非 const 方法调用不了。改成 `auto&`。

### 5. `-fno-exceptions`

serial 只用系统调用 + `Status`，没有 try/catch，因此**进子集**：
`core + math + dsp + net + io + time + serial`。子集用例 134 → **151**，本地 153/153 通过。

### 6. 验证

| 配置 | 结果 |
|---|---|
| MinGW x64 Release | **292/292**（290 用例 + 2 守卫），零告警 |
| Linux（WSL）Release | **292/292**；serial 17/17（含 9 条 PTY 真收发） |
| `-fno-exceptions` 子集 | **153/153**（含 serial 17） |
| install → `find_package` → 消费方 | 通过（`serial: 发现 1 个串口`） |

> 已知告警（非本次引入）：Linux + GCC 13 在 `Result<T>::map` 的 variant 实例化上报
> `-Wmaybe-uninitialized` 误报（`test_core.cpp` 触发，MinGW/GCC 15 不报）。属 GCC 已知问题，
> 未用 pragma 掩盖，留作后续。

## 0.1.5（2026-11-19）—— 发版闭环（CI 打包）+ CFAR 检测器

两件事，对应上一轮评估里"按投入产出排"的前两项。

### 1. 打包搬进 CI（发版闭环的最后一块）

0.1.3 起 SDK 包由 CPack 生成，但一直是**本机手工跑** `package-all.ps1` —— 也就是"四个包"
依赖这台机器恰好装了四套工具链，而且那两个脚本躺在被 gitignore 的 `assess/` 里。

- `package-all.ps1` / `verify-package.ps1` **移进 `scripts/`**（随之入库），
  `$root` 改成从 `$PSScriptRoot` 推，不再写死本机绝对路径；
- CI 新增两个 job：
  - `package-linux`：matrix{armv7, aarch64}，apt 交叉工具链 + CPack → `.tar.gz`；
  - `package-windows`：MSVC + CPack → `.zip`。
- 两个 job 的验收都是"**cpack → 解包 → 用包里的 Config 编一个外部工程**"，
  Windows 侧还会把 `sdk_consumer` 真跑一遍；
- 包名如实反映内含（CI 拿不到 OpenCV 的预编译包）：

| 包名 | 来源 | 内含 |
|---|---|---|
| `win-x64-msvc` | 本机 | ORT + OpenCV + ncnn |
| `win-x64-msvc-ort` | CI | ORT，无 OpenCV/ncnn |
| `linux-<arch>-glibc<NNN>` | 本机 | ORT + OpenCV |
| `linux-<arch>-glibc<NNN>-basic` | CI | 无 ORT/OpenCV |

> 意义：打包配方现在是 PR 级别可回归的 —— 改坏 install 规则，CI 当场红；
> 而带 ORT+OpenCV 的全量 release 资产仍由本机脚本生成（命名不同，不会混）。

### 2. CFAR 检测器（`ca_cfar` / `os_cfar`）

§10 定稿 `find_peaks` 时暴露了它的根本限制：门限是"相对全谱中位数"的绝对值，
**不知道局部噪声有多高**。调高 `min_prominence_db` 只是用同一把尺子量整条谱。

CFAR 换了个问法：每个被测 bin 用自己的邻域估计噪声，调用方给**期望虚警概率 `pfa`**。

```cpp
my::dsp::CfarOptions opt;              // guard=2, train=8, pfa=1e-4
auto det = my::dsp::ca_cfar(s, opt);   // 平均功率
auto os  = my::dsp::os_cfar(s, opt);   // 第 rank 小的参考单元
// Detection{ bin, freq, db, threshold_db, snr_db }
```

- 门限系数 `alpha = N * (pfa^(-1/N) - 1)`（N = 2*train）单独暴露为 `cfar_ca_alpha()`，
  便于调用方核算与调参：`alpha(8, 1e-4) = 12.45`（10.95 dB），`alpha(8, 1e-2) = 5.34`（7.27 dB）；
- 谱两端各 `guard+train` 个 bin 没有完整参考窗，不参与检测（文档写明）；
- 相邻超门限 bin 合并成一个检测（取最强 bin）——谱峰跨 2~3 个 bin，逐 bin 输出没意义；
- 功率域计算、参考窗复用同一块缓冲，循环里不分配；
- 参数非法返回空表（dsp 在 `-fno-exceptions` 子集里，不写 try/catch）。

实测（x64，8192 点 / nfft=4096 / 3 段平均）：

| 场景 | 结果 |
|---|---|
| 单音（1 kHz, SNR 20 dB） | CA 检出 bin 128，门限 -33.87 dBFS，局部 SNR 44.8 dB |
| 纯噪声，pfa=1e-4 | **0 个虚警** |
| 纯噪声，pfa=0.1 | 59 个（pfa 真的在控制虚警） |
| **弱音（-40 dB）紧邻强音（0 dB，间隔 4 bin）** | **CA 漏检**（门限被强音泄漏抬到 +0.68 dBFS）；**OS 检出**（-39.86 dBFS，门限 -59.67 dBFS）|

最后一行就是 OS-CFAR 存在的理由，测试把它钉住了。

### 用例与文档

- 新增 7 条 CFAR 用例（alpha 公式、单音、纯噪声 pfa 对比、OS vs CA、边缘、非法参数、余量）；
  总用例 266 → **273**；
- `docs/08` 新增 §11「CFAR 检测」（CA/OS 选择建议、alpha 数值例、边缘约定、已知限制：
  OS 复用 CA 的 alpha 属保守近似、极短谱无法检测）；
- `docs/03` 新增 §4.4.1「CI 打包 job」；README 的 SDK 包表补上来源与内含对照。

### 验证矩阵

| 配置 | 结果 |
|---|---|
| MinGW x64 Release | **275/275**（273 用例 + 2 守卫），零告警 |
| Linux Debug / ASan / TSan | 见 CI（14 个 job，含 qemu-test 两个 ARM 真跑） |
| `-fno-exceptions` | CFAR 实现无 try/catch，子集里语法检查通过 |
| CI 打包 job | 新增，随本版首跑验证 |

## 0.1.4（2026-11-19）—— find_peaks 语义定稿

### 问题

`find_peaks(s)` 声明里写的是"局部极大 + 高于噪声底 min_prominence_db + 最小间隔"，
但名字叫 `prominence`，实现的却是"高出噪声底"这种**绝对**门限。后果实测：

```
1 kHz 单音 + 平坦噪声底（fs=8k, nfft=1024, Hann）：
  返回 8 个峰，7 个在 -133~-139 dBFS，peaks.front() = 320.3 Hz
  真峰（1000 Hz, 0 dBFS）排在第 4 个
```

### 决定

1. **`min_prominence_db` 改成真·prominence**（地形学/光谱学的标准定义，
   与 `scipy.signal.find_peaks` 的 `prominence` 同义）：
   `prominence = db[k] - max(left_valley, right_valley)`，
   谷底取"到最近更高 bin 为止"这一路的最小值。
2. **保留"高出噪声底"作为前置粗筛**，两者是 AND：粗筛便宜且能挡住"整段贴底"，
   真·prominence 才是判据。
3. **输出仍按频率升序**（与 scipy 一致）：要最强的峰用 `Spectrum::peak_*`。
   `max_peaks` 截断保留**最强**的几个（内部先按 dB 排序）。
4. **`Peak` 新增 `prominence_db` 字段**，调用方可以按自己的门限再过滤。

### 行为变化（0.x 内的破坏性改动，写清楚）

| 输入 | 0.1.3 | 0.1.4 |
|---|---|---|
| 1 kHz 单音 | 8 个峰（7 个假峰） | 1 个峰 |
| 纯白噪声（7 段 Welch 平均） | 8 个噪声峰 | 0 个 |
| 4 个音 + `max_peaks=2` | 更强的两个 | 更强的两个（不变） |

### 用例

`tests/test_dsp.cpp` 的 find_peaks 相关用例从 2 条扩到 7 条：
单音必须只有 1 个峰（旧实现在这条上必然失败）、双音按频率升序、纯噪声 0 个峰、
`max_peaks` 保留最强、`min_sep` 合并相近峰，外加一条**把已知限制钉住**的用例
（Welch 平均段数很少时 6 dB 门限会放进噪声峰，12 dB 才干净）。

### 已知限制（写进 docs/08 §10）

- Welch 平均段数很少时，单个噪声 bin 可能比中位数高 6 dB 以上 → 调高门限；
- 完全没有噪声的合成谱里，FFT 舍入起伏（-150~-160 dB）的 prominence 可达 20~30 dB
  → 仿真时加一点噪声；
- 真正抗噪的检测器要 CFAR，仍列在 docs/08 §8 的待办里。

`docs/08-信号处理与解调.md` 新增 §10「峰值检测的语义」：判据、输出顺序、
两张限制表、新旧实现对照的实测数据。

## 0.1.3（2026-11-18）—— 可复现打包 + Int8 实测 + DSP 流式处理

三件事：把 SDK 包变成"一条命令能重建"、把 Int8 量化从"性价比第一名（无数字）"变成实测结论、
把 `dsp` 从"只能整段批处理"补上流式状态对象。

### 1. 打包：CPack + 包内许可证（`cmake/Packaging.cmake`）

评估报告里 `dist/` 的两条问题（C1 不可重建、C2 包内零许可证）在这版修掉。

- CPack 直接打 `install` 树：Windows 出 ZIP、Linux 出 TGZ，
  产物 `my_cpp_libs-<版本>-<suffix>.zip|.tar.gz`，顶层目录也带版本；
- **包名后缀强制显式给**（`-DMYLIB_PACKAGE_SUFFIX=`）：glibc 版本 CMake 推不出来，
  "用 2.39 编的包装到 2.30 的板子上"是最容易犯的错；
- `CPACK_PACKAGE_DESCRIPTION` 写入 git commit / 编译器 / 构建类型 / 组件清单；
- 包内 `share/licenses/my_cpp_libs/`：我们的 MIT + spdlog/fmt/nlohmann/Eigen/httplib/
  ORT（含 `ThirdPartyNotices.txt`）/OpenCV/ncnn 的原始许可证；stb 与 pocketfft 没有独立
  LICENSE 文件（声明在头文件里），直接把源文件装进 `licenses/sources/`；
- 缺任何一份许可证会打印 `[pkg] 找不到许可证 ...`，作为发布前的红灯；
- 新增 `docs/THIRD_PARTY_NOTICES.md`（逐个依赖的许可证/再分发注意事项）。

四个包全部验证过"解开 → `find_package` → 编 → 跑"（Windows 两个真跑，交叉包检查 ELF 架构/`.so` 依赖）。

### 2. Int8 量化：从口号到数字（`tools/quantize_models.py` + [docs/12](docs/12-量化实测.md)）

用**我们自己的 `model_inspect`**（`my::infer` + ORT）在 x86_64 实测：

| 模型 | fp32 | Int8 (QDQ) | 加速 | 体积 |
|---|---|---|---|---|
| `small_cnn` 1×3×32×32 | 0.054 ms | 0.088 ms | **0.61×（更慢）** | 22 → 11 KB（2.03×）|
| `bench_cnn` 1×3×224×224 | 6.83 ms | 5.03 ms | **1.36×** | 551 → 149 KB（3.69×）|
| `mobilenetv2-12` | 11.27 ms | 5.55 ms | **2.03×** | 13.6 MB → 3.7 MB（3.64×）|

顺带纠正了三个假设：

1. **小模型上量化会更慢**（Q/DQ 开销 > 计算节省），32×32 模型实测 0.61×；
2. **合成校准数据不可信**：MobileNet 用合成数据校准后 top-1 一致率 0~50%、
   余弦 0.91~0.97（三种合成分布都试过）。精度必须用真实图片重跑 `--calib-dir`；
3. **ARMv7 上别照搬 x86 数字**：Cortex-A9 没有 int8 点积指令，
   `docs/04 §8` 里"Zynq 上 2~3×"应改成条件句，且必须先上板测。

另外两个"量化阶段不报错、加载时才炸"的坑也写进了文档与脚本：
opset < 13 的模型必须先升 opset（`Q/DQ` 的 `axis` 属性是 opset 13 引入的），
以及 `quantize_dynamic` 对 CNN 会产出 ORT 未实现的 `ConvInteger`。

### 3. DSP 流式处理（`include/my_cpp_libs/dsp/stream.hpp`）

板端是"每帧一块"的，而 `dsp` 之前只有整段批处理 API。新增 5 个跨块保持状态的对象：

| 类 | 保持的状态 | 与批量版的关系 |
|---|---|---|
| `FirFilter` | 延迟线 | `process()` 逐块 + `flush()` == `fir_apply()` 整段，逐点一致 |
| `BiquadFilter` | 每级转置直接 II 型的 z1/z2 | == 逐级 `biquad_apply()` |
| `ComplexMixer` | NCO 相位累加器 | == `mix_down()`（相位跨块连续，这是流式混频最容易写错的地方）|
| `Decimator` | FIR 状态 + 抽取相位 | == `decimate()`（符号数与数值都一致）|
| `Agc` | 增益 + 功率估计 | 新增（批量版没有）|

新增 10 个用例（套件 `dsp_stream`），核心是"同一段信号分别走批量与流式两条路，
逐点 `CHECK_NEAR`"——不是"看着对"，是钉住的。0 长度块、1 长度块、不规则块长、
非法参数都覆盖。`stream.cpp` 在 `-fno-exceptions` 下也能编（`dsp` 在子集里）。

### 4. ARM 单测真跑（qemu CI）+ 一条命令发布

**`qemu-test` job（CI）**：在此之前"5 个平台"里的 ARM 证据只到"能编能链"。
新增的 job 用 `qemu-arm-static` / `qemu-aarch64-static` 把 armv7 与 aarch64 的单测**真的跑一遍**。

要做到"能跑"有两个关键选择（都验证过）：

- 产物链接成**全静态**（`-DCMAKE_EXE_LINKER_FLAGS=-static`）→ qemu 直接执行，
  **不需要 `-L <sysroot>`**。本地实测 armv7 产物 4.45 MB / aarch64 19.7 MB，
  `readelf -d` 输出 "no dynamic section"；
- **不注册 binfmt**，把 `qemu-*-static` 当解释器直接调用，绕开 `update-binfmts` 的权限问题。

job 里还有一道防退化检查：`readelf -d | grep NEEDED` 命中就判失败，
避免"`-static` 悄悄失效"变成一堆看不懂的 qemu 报错。

**`scripts/release.ps1`**：把"发布"固定成一条命令 —— 读版本号 → 推 `main`+tag →
用 CHANGELOG 章节当 notes 建 release → 上传 `dist/` 下的包 → 拉回 release 核对字节数。
token 只从 `$env:GH_TOKEN` 读，不落盘。

> 这轮踩到一个权限坑：GitHub 的细粒度 PAT 如果没有 **Workflows: Read and write**，
> 任何"改动 `.github/workflows/` 的提交"都会被拒绝推送
> （`refusing to allow a Personal Access Token to create or update workflow ...`），
> 与内容是否正确无关。另外本机 `git config http.proxy=127.0.0.1:7897` 会让
> Git for Windows 的 schannel 握手失败，脚本里改用 `-c http.sslBackend=openssl`。

### 5. 发布后发现并修掉的问题（同版补充）

v0.1.3 推上去之后 CI 首跑 12/14 绿（两个 QEMU job 都过了），两个红，都已定位：

| job | 现象 | 根因 | 修法 |
|---|---|---|---|
| `linux-noexceptions` | Configure 阶段 FATAL_ERROR | 0.1.2 起 `THREAD` 进了 fail-closed 名单，但 CI / `ci-local.ps1` / `docs/03` 的示例命令还停在旧参数（只关到 `HTTP=OFF`） | 三处补 `-DMYLIB_ENABLE_THREAD=OFF`，标题同步成 core+math+dsp+net+io+time |
| `linux-ubsan` | `thread_pool_runs_all_tasks` 0.00s 失败 | **测试断言了库并不保证的性质**：`f.get()` 只保证任务体返回，`pending_` 要等 packaged_task 兑现 promise 之后才自减 | 测试改用 `wait_idle()` 做屏障；头文件写明"`wait_idle()` 才是全部完成的可靠屏障"；worker_loop 加注释防误改 |

第二次跑 TSan 又抓到一条（时好时坏）：

```
WARNING: ThreadSanitizer: data race
  Write of size 8 ... operator delete              <- spdlog worker 线程
  Previous atomic write of size 4 ... _M_release   <- 主线程 my::log::shutdown()
  stack: async_msg::~async_msg / thread_pool::process_next_msg_
```

队列里每条 `async_msg` 都持有一份 logger 的 `shared_ptr`，所以 `shutdown()` 释放自己那份时，
最后一份引用可能由 worker 线程释放 —— 引用计数本身是原子的（安全），但 TSan 把它判成 race。
修法两条：

1. `shutdown()` 改为**先按 `queue_size()` 有界排空，再释放 logger**（spdlog 没有停止接口，
   `post_async_msg_` 是私有的，只能等）；
2. 新增 `tests/tsan.supp`，**按栈帧**窄化抑制 spdlog 这条销毁路径 —— 我们自己的代码一条都不抑制，
   文件里逐条写了"为什么它不是真问题"。CI 的 `linux-tsan` 通过
   `TSAN_OPTIONS=...:suppressions=$${workspace}/tests/tsan.supp` 加载（必须绝对路径，
   ctest 的工作目录是 `build/ci-tsan`）。

因为 `libmy_cpp_libs_log` 变了，v0.1.3 的四个 SDK 包**重新构建并重新上传**，
release 的 tag 也移到修复后的提交上 —— 发布出去的包与仓库代码保持一致。

### 验证矩阵

| 配置 | 结果 |
|---|---|
| MinGW x64 Release | **263/263**（261 用例 + 2 守卫） |
| MSVC x64 Release（ORT + ncnn + OpenCV） | **271/271**（261 + cv 8 + 2） |
| Linux gcc Debug（断言全开） | **263/263** |
| Linux ASan（Debug + 泄漏检测） | 261 用例 0 失败（8 个 ml 显式跳过） |
| Linux TSan | **263/263，无数据竞争** |
| `-fno-exceptions`（core+math+dsp+net+io+time） | **124/124**（含 dsp_stream 10 个） |
| `install` → `find_package`（4 个包） | 全部解开→编→跑通过 |

## 0.1.2（2026-11-18）—— 工程底座：io / time / thread / 分层配置

0.1.1 把安全网补上之后，这一版开始按 roadmap 的 P1 顺序补组件。四个都只依赖标准库
（`io`/`time` 还进了 `-fno-exceptions` 子集），目标是消除"每个工程都重写一遍、而且
每次重写都会踩同一个坑"的那部分代码。

### 新组件

| 组件 | target | 解决的问题 | 关键点 |
|---|---|---|---|
| `my::io` | `my_cpp_libs::io` | 文件/目录/路径/临时文件 | **`write_file` 是原子的**（临时文件 + rename）；全部走 `error_code` 重载，不抛异常 |
| `my::time` | `my_cpp_libs::time` | 时钟、时间戳、计时 | 单调时钟测耗时、UTC 格式化、`Stopwatch`/`ScopedTimer` |
| `my::thread` | `my_cpp_libs::thread` | 线程池/阻塞队列/周期任务 | 析构必 drain+join；有界队列；`PeriodicTask` 固定速率不追补 |
| `my::json::Layered` | 复用 `my_cpp_libs::json` | 分层配置 | 默认值 ← 文件 ← 环境变量 ← 命令行；对象深合并、数组整体覆盖 |

分层配置**没有**独立 target：它就是"JSON 的深合并"，独立组件只会多一层类型转换。
详见 [docs/11](docs/11-基础组件.md)。

### 修掉的并发缺陷（测试抓到的）

`my::thread::ThreadPool::shutdown()`（以及析构函数）在**锁外**置位 `stopping_`，
与工作线程的条件变量等待之间存在**丢失唤醒**窗口：

```
worker: 持锁判谓词(假) → 释放锁 → 尚未进入 cond_wait 的注册
main  : 锁外 stopping_=true → notify_all()  ← 通知落在"注册之前"，被丢弃
worker: 睡下；谓词已是 true，但再没有人叫醒它 → join 永久阻塞
```

实测约 5~25% 概率整个进程挂死（ctest 报 300s 超时）。用 gdb 附加抓到确凿栈：
worker 停在 `work_cv_.wait`、主线程停在 `shutdown()` 的 `join`。
修法是标准写法：**改状态和 notify 必须与等待方共用同一把 mutex**。
修完之后同一用例连跑 200 次、全量连跑 3 轮均无挂起。

### 顺带

- `BuildInfo::hasExceptions()`：`-fno-exceptions` 构建可以据此跳过"回调抛异常要被吞掉"
  这类只有异常开启时才有意义的用例；`summary()` 也会打上 `| no-exceptions`。
- `ScopedTimer` 的析构按"异常是否开启"裁剪 try/catch（否则 `-fno-exceptions` 编不过）。
- 测试框架：`[ RUN ]` 之后 `fflush(stdout)` —— 重定向到文件时是块缓冲，用例一旦挂住
  被 kill，日志里连"是哪个用例"都看不到（这次排查就吃了这个亏）。
- `my_cpp_libs.hpp` 汇总头、SDK 的 Config、`ci-local.ps1`、`tests/sdk_consumer` 均已覆盖新组件。

### 规模

- 新增 52 个用例：io 13 / time 11 / thread 16 / config 12，总用例 199 → **251**。
- 新增示例 `example_app_skeleton`：配置 → 日志 → 线程池 → 周期任务 → 原子写，
  五个组件串起来的"最小可用服务骨架"。

### 验证矩阵（实测）

| 配置 | 结果 |
|---|---|
| MinGW x64 Release | **253/253**（251 用例 + 2 守卫），连跑 3 轮无挂起 |
| MSVC x64 Release（ORT + ncnn + OpenCV 4.12） | **261/261**（251 + cv 8 + 2），带 `--fail-on-skip` |
| Linux gcc **Debug**（断言全开） | **253/253** |
| Linux ASan（Debug + 泄漏检测） | 251 用例 0 失败（8 个 ml 显式跳过） |
| Linux **TSan** | **253/253，无数据竞争报告**（0.1.2 开发中它先抓到了用例自己的 `raw` 指针竞争） |
| `-fno-exceptions`（core+math+dsp+net+**io**+**time**） | **114/114**，`-fno-exceptions` 确在命令行、`libmy_cpp_libs_io.a` 编出来了 |
| 最小配置（只留 core） | 只出 1 个静态库 |
| `install` → `find_package`（外部工程） | 编译 + **运行**通过，`io/time/thread` 已随 SDK 导出 |
| `example_app_skeleton` | 跑通：配置 3 层合并、线程池批处理 ~0.4 ms、周期任务 3 次心跳、结果原子写盘 |

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
