# resources —— 三方库原始压缩包

这个目录存放项目依赖的**原始压缩包**。`cmake/Deps.cmake` 在首次 configure 时会把它们
自动解压到 `third_party/`，所以克隆仓库后只要把下面这些包放进来就能直接构建，
**不需要联网**。

> **这些 zip 不进 git**（`.gitignore` 里排除了 `resources/*.zip`）。
> 单个包最大 75 MB，进仓库会让 clone 变得很慢；而且它们都能从上游重新下载。

## 包清单（7 个：6 个必需 + ORT 可选）

| 压缩包 | 解压到 | 体积 | 下载地址 |
|---|---|---|---|
| `eigen-5.0.0.zip` | `third_party/eigen-5.0.0/` | 3.8 MB | https://gitlab.com/libeigen/eigen/-/archive/5.0.0/eigen-5.0.0.zip |
| `spdlog-1.17.0.zip` | `third_party/spdlog-1.17.0/` | 0.3 MB | https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.zip |
| `json-include.zip` | `third_party/nlohmann-json-3.12.0/` | 0.3 MB | https://github.com/nlohmann/json/releases/download/v3.12.0/include.zip |
| `stb-image-2.32.zip` | `third_party/stb-image-2.32/` | 0.09 MB | https://github.com/nothings/stb （pin 见下） |
| `pocketfft-4d865cb.zip` | `third_party/pocketfft-4d865cb/` | 0.02 MB | https://github.com/mreineck/pocketfft （pin 见下） |
| `httplib-0.66.0.zip` | `third_party/httplib-0.66.0/` | 0.19 MB | https://github.com/yhirose/cpp-httplib （pin 见下） |
| `onnxruntime-win-x64-1.23.2.zip` | `third_party/onnxruntime-win-x64-1.23.2/` | 74.5 MB | https://github.com/microsoft/onnxruntime/releases/download/v1.23.2/onnxruntime-win-x64-1.23.2.zip |

> `json-include.zip` 解压后没有顶层目录，`Deps.cmake` 会把它直接摊到 `third_party/json/`。
> `stb-image-2.32.zip` / `pocketfft-4d865cb.zip` 同理（里面是单头文件），
> 由 `scripts/fetch-deps.*` 现场生成。

### 单头文件库为什么要自己打包

stb 和 pocketfft 上游都没有发布 tag，也没有现成 zip，只有仓库里的头文件。
所以本工程**按 commit 固定 + SHA256 校验**，再由 fetch 脚本打成本工程约定的 zip：

| 库 | commit | 文件 | SHA256 |
|---|---|---|---|
| stb | `9600522de53973336fa033a289858a4354cbb04c` | `stb_image.h` (v2.32) | `DEB4F18C…AAB6E91` |
| stb | 同上 | `stb_image_write.h` (v1.18) | `A8E7F77F…0D9E7CA` |
| pocketfft | `4d865cba24359b05b37da4e75aee24498c3b78e7` | `pocketfft_hdronly.h` | `518A95C5…744C86D` |
| httplib | `cdcd2ac6e6d5548b5a9130fb39ab76d2c6c614b0` | `httplib.h` + `LICENSE` | `51DDD148…97EA1C35` |

换版本要同时改 `scripts/fetch-deps.ps1`、`scripts/fetch-deps.sh` 里的
PIN / SHA256 / 文件名三处，否则校验会直接把不匹配的包拦下来。

## 可选的包（按需）

这些**不参与自动解压**，需要手动放到 `third_party/` 或用 `-D<NAME>_ROOT=` 指定：

| 目录 | 用途 | 来源 |
|---|---|---|
| `third_party/ncnn-src/` | ncnn 源码（MSVC/Linux 下编译） | `git clone https://github.com/Tencent/ncnn` |
| `third_party/ncnn-20250503/` | ncnn Windows 预编译包 | https://github.com/Tencent/ncnn/releases |
| `third_party/ncnn-linux-armv7-20250503/` | ncnn armv7 交叉编译产物 | 见 [docs/03-构建与交叉编译.md](../docs/03-构建与交叉编译.md) |
| `third_party/opencv-4.12.0/` | OpenCV（约 475 MB） | https://opencv.org/releases/ 或 `-DOpenCV_DIR=` 指向已有安装 |
| `third_party/onnxruntime-armv7-petalinux2020/` | 板端用的 ORT 1.18.1 | 见 [docs/06-板端验证手册.md](../docs/06-板端验证手册.md) |
| `third_party/opencv-4.12.0-armv7/` | 板端用的 OpenCV（core+imgproc+imgcodecs） | 由 `scripts/build-opencv-armv7-petalinux2020.sh` 在 VM 上编出，见 [docs/05](../docs/05-OpenCV接入方案.md) |

## 一次性下载脚本

PowerShell（在仓库根目录执行）：

```powershell
$urls = @{
  'eigen-5.0.0.zip'                  = 'https://gitlab.com/libeigen/eigen/-/archive/5.0.0/eigen-5.0.0.zip'
  'spdlog-1.17.0.zip'                = 'https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.zip'
  'json-include.zip'                 = 'https://github.com/nlohmann/json/releases/download/v3.12.0/include.zip'
  # ORT 是可选的（缺了 infer 回退 stub），按平台自备
  'onnxruntime-win-x64-1.23.2.zip'   = 'https://github.com/microsoft/onnxruntime/releases/download/v1.23.2/onnxruntime-win-x64-1.23.2.zip'
}
New-Item -ItemType Directory -Force resources | Out-Null
foreach ($k in $urls.Keys) {
  Write-Host "下载 $k ..."
  Invoke-WebRequest -Uri $urls[$k] -OutFile "resources\$k"
}
```

然后正常 configure 即可：

```powershell
cmake -S . -B build/win-msvc --preset win-msvc
```
