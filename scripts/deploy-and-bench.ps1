<#
.SYNOPSIS
    把 armv7 产物打包/部署到板子上并跑一遍基准测试。

.DESCRIPTION
    两种模式：
      -Package   只在本地打一个 tar.gz（可以拷到 SD 卡上带过去）
      -Deploy    通过 scp 推到板子，ssh 上跑测试和基准

    部署内容：mylib_tests、model_inspect、libonnxruntime.so.1、测试模型。

.EXAMPLE
    # 先生成离线包（板子还没到手时也能做）
    .\scripts\deploy-and-bench.ps1 -Package

.EXAMPLE
    .\scripts\deploy-and-bench.ps1 -Board 192.168.1.50 -User root -Model models\real\mobilenetv2-12.onnx
#>
[CmdletBinding(DefaultParameterSetName = 'Package')]
param(
    [Parameter(ParameterSetName = 'Package')]
    [switch] $Package,

    [Parameter(ParameterSetName = 'Deploy', Mandatory = $true)]
    [string] $Board,

    [Parameter(ParameterSetName = 'Deploy')]
    [string] $User = 'root',

    [Parameter(ParameterSetName = 'Deploy')]
    [int] $Port = 22,

    [string] $Preset = 'linux-armv7',
    [string] $Model  = 'models\small_cnn.onnx',
    [switch] $SkipBuild,
    [string] $RemoteDir = '/tmp/mylib'
)

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$env:PATH = "D:\ProgramFiles\ninja-win;$env:PATH"

# -----------------------------------------------------------------------------
#  1. 构建
# -----------------------------------------------------------------------------
if (-not $SkipBuild) {
    Write-Host "==> 构建 $Preset" -ForegroundColor Cyan
    cmake --preset $Preset 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "configure 失败" }
    cmake --build --preset $Preset 2>&1 | Select-Object -Last 3
    if ($LASTEXITCODE -ne 0) { throw "build 失败" }
}

$binDir = Join-Path $root "build\$Preset\bin"
if (-not (Test-Path $binDir)) { throw "找不到 $binDir" }

# -----------------------------------------------------------------------------
#  2. 组装部署目录
# -----------------------------------------------------------------------------
$stage = Join-Path $root "build\deploy-$Preset"
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $stage | Out-Null

$files = @(
    @{ Src = "$binDir\mylib_tests";      Dst = 'mylib_tests' },
    @{ Src = "$binDir\model_inspect";    Dst = 'model_inspect' },
    @{ Src = "$binDir\example_pipeline"; Dst = 'example_pipeline' }
)
foreach ($f in $files) {
    if (Test-Path $f.Src) {
        Copy-Item $f.Src (Join-Path $stage $f.Dst) -Force
    } else {
        Write-Warning "缺少 $($f.Src)"
    }
}

# ONNX Runtime 动态库（SONAME 是 libonnxruntime.so.1，名字不能改）
$ort = Join-Path $root 'third_party\onnxruntime-linux-armv7-1.23.2\lib\libonnxruntime.so.1'
if (Test-Path $ort) {
    Copy-Item $ort (Join-Path $stage 'libonnxruntime.so.1') -Force
} else {
    Write-Warning "没有找到 armv7 版 libonnxruntime.so.1，板端将无法用 onnxruntime 后端"
}

# 模型
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'models') | Out-Null
foreach ($m in @('small_cnn.onnx', 'dyn_batch.onnx', 'multi_io.onnx')) {
    $p = Join-Path $root "models\$m"
    if (Test-Path $p) { Copy-Item $p (Join-Path $stage 'models') -Force }
}
if (Test-Path 'models\ncnn') {
    Copy-Item 'models\ncnn' (Join-Path $stage 'models\ncnn') -Recurse -Force
}
if ($Model -and (Test-Path $Model)) {
    Copy-Item $Model (Join-Path $stage 'models') -Force
}

# 运行说明
@"
# my_cpp_libs 板端验证

export LD_LIBRARY_PATH=`$PWD:`$LD_LIBRARY_PATH

# 1) 单元测试（63 个用例）
./mylib_tests

# 2) 模型体检 + 延迟
./model_inspect models/small_cnn.onnx --run --bench 10

# 3) 完整流水线（预处理 -> 推理 -> 后处理）
./example_pipeline models/small_cnn.onnx

# 4) 记录环境
uname -a
cat /proc/cpuinfo | grep -E 'model name|BogoMIPS|Features' | head -4
free -m
"@ | Set-Content (Join-Path $stage 'README.txt') -Encoding utf8

$sizeMb = [math]::Round(((Get-ChildItem $stage -Recurse -File | Measure-Object Length -Sum).Sum / 1MB), 1)
Write-Host "==> 部署目录: $stage ($sizeMb MB)" -ForegroundColor Cyan
Get-ChildItem $stage | Select-Object Name, @{n='KB';e={[math]::Round($_.Length/1KB,1)}} | Format-Table -AutoSize | Out-Host

# -----------------------------------------------------------------------------
#  3a. 打包模式
# -----------------------------------------------------------------------------
if ($PSCmdlet.ParameterSetName -eq 'Package') {
    $tarPath = Join-Path $root "build\mylib-$Preset.tar.gz"
    Remove-Item $tarPath -Force -ErrorAction SilentlyContinue
    # Windows 自带的 bsdtar；-C 指定根目录，包内是相对路径
    tar -czf $tarPath -C $stage .
    if ($LASTEXITCODE -ne 0) { throw "打包失败" }
    Write-Host ""
    Write-Host "已生成: $tarPath" -ForegroundColor Green
    Write-Host "拷到板子上后："
    Write-Host "  mkdir -p /tmp/mylib && tar -xzf mylib-$Preset.tar.gz -C /tmp/mylib && cd /tmp/mylib && ./mylib_tests"
    exit 0
}

# -----------------------------------------------------------------------------
#  3b. 部署模式
# -----------------------------------------------------------------------------
$target = "$User@$Board"
Write-Host "==> 部署到 $target : $RemoteDir" -ForegroundColor Cyan

$sshOpts = @('-P', $Port, '-o', 'StrictHostKeyChecking=accept-new')

ssh @sshOpts $target "mkdir -p $RemoteDir"
if ($LASTEXITCODE -ne 0) { throw "ssh 连接失败" }

scp -P $Port -r (Join-Path $stage '*') "${target}:$RemoteDir/"
if ($LASTEXITCODE -ne 0) { throw "scp 失败" }

Write-Host ""
Write-Host "==> 板端执行" -ForegroundColor Cyan

$remote = @"
cd $RemoteDir && chmod +x mylib_tests model_inspect example_pipeline && \
export LD_LIBRARY_PATH=`$PWD:`$LD_LIBRARY_PATH && \
echo '===== 环境 =====' && uname -a && (grep -E 'model name|Features' /proc/cpuinfo | head -2) && free -m | head -2 && \
echo '===== 单元测试 =====' && ./mylib_tests | tail -3 && \
echo '===== 推理基准 =====' && ./model_inspect models/small_cnn.onnx --run --bench 10 | tail -6
"@

ssh @sshOpts $target "bash -lc '$($remote -replace "'", "`'")'"

Write-Host ""
Write-Host "完成。把上面的输出贴回来，我们就能定板端用 ORT 还是 ncnn。" -ForegroundColor Green
