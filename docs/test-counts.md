# 测试用例数（自动生成，不要手改）

> 单一来源：`mylib_tests --list` 的输出。
> 重新生成：`python tools/gen_test_counts.py --exe <mylib_tests> --write`
> CI：`python tools/gen_test_counts.py --exe <mylib_tests> --check`（数字漂移直接失败）。
>
> 这张表是"全组件构建"（含 cv）的数字；关掉 cv 的构建少 12 条。
> 其它文档引用用例数时请指向本文件，不要手写。

| 套件 | 用例数 |
|---|---:|
| `codec` | 6 |
| `config` | 12 |
| `core` | 15 |
| `crash` | 2 |
| `cv` | 12 |
| `dsp` | 52 |
| `dsp_stream` | 11 |
| `framework` | 5 |
| `gaps` | 12 |
| `hash` | 9 |
| `http` | 25 |
| `image` | 24 |
| `infer` | 17 |
| `io` | 15 |
| `json` | 18 |
| `log` | 17 |
| `math` | 15 |
| `ml` | 14 |
| `net` | 32 |
| `net_server` | 8 |
| `preprocess` | 28 |
| `serial` | 19 |
| `thread` | 19 |
| `time` | 11 |

合计：**398** 条（`mylib_tests --list` 的去重计数；CTest 另加 2 条守卫）。

可选用例：`cv`（12 条）只在 `MYLIB_ENABLE_CV=ON` 时注册；`ml` 在没有推理后端时
会整组 SKIP，但条数不变。
