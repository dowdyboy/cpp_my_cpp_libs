<#
.SYNOPSIS
    在本机跑一遍 CI 的等价检查（不需要 GitHub）。

.DESCRIPTION
    对应 .github/workflows/ci.yml 里能在 Windows 上跑的部分：

        default   默认全组件 + 单元测试
        ubsan     MYLIB_SANITIZE=undefined（trap 模式，MinGW 可用）
        minimal   只留 core+log+image，验证组件开关真的能裁剪
        noex      MYLIB_NO_EXCEPTIONS=ON（core+math+dsp+net 子集），含反向校验
consumer  examples/consumer_app 独立工程：add_subdirectory 嵌入后能编能跑
        msvc      MSVC 全组件（有 OpenCV 时用例更多）

    交叉编译（armv7 / aarch64）用 ..\scripts\build-all.ps1 跑，那两个只验证能编能链。

.EXAMPLE
    .\scripts\ci-local.ps1
    .\scripts\ci-local.ps1 -Only default,ubsan
    .\scripts\ci-local.ps1 -Clean
#>
[CmdletBinding()]
param(
    [string[]] $Only,
    [switch]   $Clean
)

# 和 build-all.ps1 一样：不能用 'Stop'，PowerShell 5.1 会把原生命令的 stderr 当致命错误
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$env:PATH = "D:\ProgramFiles\ninja-win;D:\ProgramFiles\msys64\ucrt64\bin;$env:PATH"

$mingwCxx = 'D:/ProgramFiles/msys64/ucrt64/bin/g++.exe'
$ninja    = 'D:/ProgramFiles/ninja-win/ninja.exe'

$cases = @(
    @{ Name = 'default'; Args = @('-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                                 "-DCMAKE_CXX_COMPILER=$mingwCxx", "-DCMAKE_MAKE_PROGRAM=$ninja") },
    @{ Name = 'ubsan';   Args = @('-G', 'Ninja', '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
                                 '-DMYLIB_SANITIZE=undefined',
                                 "-DCMAKE_CXX_COMPILER=$mingwCxx", "-DCMAKE_MAKE_PROGRAM=$ninja") },
    @{ Name = 'minimal'; Args = @('-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                                 '-DMYLIB_ENABLE_JSON=OFF', '-DMYLIB_ENABLE_MATH=OFF',
                                 '-DMYLIB_ENABLE_INFER=OFF',
                                 "-DCMAKE_CXX_COMPILER=$mingwCxx", "-DCMAKE_MAKE_PROGRAM=$ninja") },
    @{ Name = 'noex';    Args = @('-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                                 '-DMYLIB_NO_EXCEPTIONS=ON',
                                 '-DMYLIB_ENABLE_LOG=OFF', '-DMYLIB_ENABLE_JSON=OFF',
                                 '-DMYLIB_ENABLE_INFER=OFF', '-DMYLIB_ENABLE_IMAGE=OFF',
                                 '-DMYLIB_ENABLE_CV=OFF', '-DMYLIB_ENABLE_HTTP=OFF',
                                 "-DCMAKE_CXX_COMPILER=$mingwCxx", "-DCMAKE_MAKE_PROGRAM=$ninja") },
    @{ Name = 'consumer'; Source = 'examples/consumer_app';
       Args = @('-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                "-DCMAKE_CXX_COMPILER=$mingwCxx", "-DCMAKE_MAKE_PROGRAM=$ninja");
       Run = 'consumer_app.exe' },
    @{ Name = 'msvc';    Args = @('-A', 'x64') }
)

if ($Only) { $cases = $cases | Where-Object { $Only -contains $_.Name } }

$results = @()
foreach ($c in $cases) {
    $name = $c.Name
    $dir  = "build/ci-local-$name"
    Write-Host ""
    Write-Host ('=' * 70) -ForegroundColor Cyan
    Write-Host "  CI 等价检查: $name" -ForegroundColor Cyan
    Write-Host ('=' * 70) -ForegroundColor Cyan

    $status = 'OK'; $note = ''
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        if ($Clean) { Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue }
        $src = '.'
        if ($c.Source) { $src = $c.Source }
        cmake -S $src -B $dir @($c.Args) 2>&1 | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "configure 失败 (exit $LASTEXITCODE)" }

        if ($name -eq 'msvc') {
            cmake --build $dir --config Release 2>&1 | Out-Host
        } else {
            cmake --build $dir 2>&1 | Out-Host
        }
        if ($LASTEXITCODE -ne 0) { throw "build 失败 (exit $LASTEXITCODE)" }

        if ($c.Run) {
            # 嵌入验证：独立工程没有 ctest，直接把消费方程序跑起来（返回非 0 即失败）
            $exe = Join-Path $dir $c.Run
            if (-not (Test-Path $exe)) { throw "找不到消费方产物: $exe" }
            & $exe 2>&1 | Out-Host
            if ($LASTEXITCODE -ne 0) { throw "消费方程序失败 (exit $LASTEXITCODE)" }
        } else {
            if ($name -eq 'msvc') {
                ctest --test-dir $dir -C Release --output-on-failure 2>&1 | Out-Host
            } else {
                ctest --test-dir $dir --output-on-failure 2>&1 | Out-Host
            }
            if ($LASTEXITCODE -ne 0) { throw "ctest 失败 (exit $LASTEXITCODE)" }
        }

        if ($name -eq 'minimal') {
            # 裁剪必须真的生效，而不是"编过了但库还在"
            $json = Get-ChildItem $dir -Recurse -Filter 'libmy_cpp_libs_json*' -ErrorAction SilentlyContinue
            $log  = Get-ChildItem $dir -Recurse -Filter 'libmy_cpp_libs_log*'  -ErrorAction SilentlyContinue
            $img  = Get-ChildItem $dir -Recurse -Filter 'libmy_cpp_libs_image*' -ErrorAction SilentlyContinue
            if ($json) { throw 'MINIMAL 检查失败：json 组件仍然被构建了' }
            if (-not $log) { throw 'MINIMAL 检查失败：log 组件没有产物' }
            # image 不依赖 infer / json / math：关掉那三个之后它仍然要能编出来
            if (-not $img) { throw 'MINIMAL 检查失败：image 组件没有产物（它不该依赖 infer）' }
        }

        if ($name -eq 'noex') {
            # ① 标志必须真的进了编译命令行，而不是"选项存在但没人用"
            $ninjaFile = Join-Path $dir 'build.ninja'
            if (-not (Select-String -Path $ninjaFile -Pattern 'fno-exceptions' -Quiet)) {
                throw 'NOEX 检查失败：build.ninja 里没有 -fno-exceptions'
            }
            # ② 与默认开启的 json 同开时必须 configure 失败（fail-closed）
            $badDir = 'build/ci-local-noex-bad'
            Remove-Item -Recurse -Force $badDir -ErrorAction SilentlyContinue
            cmake -S . -B $badDir @('-G', 'Ninja', '-DMYLIB_NO_EXCEPTIONS=ON') *> $null
            if ($LASTEXITCODE -eq 0) {
                throw 'NOEX 反向校验失败：开着 json 时 configure 竟然成功了'
            }
            Remove-Item -Recurse -Force $badDir -ErrorAction SilentlyContinue
        }
    } catch {
        $status = 'FAIL'
        $note   = $_.Exception.Message
    }
    $sw.Stop()

    $results += [pscustomobject]@{
        Case = $name; Status = $status
        Seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1); Note = $note
    }
}

Write-Host ""
Write-Host ('=' * 70) -ForegroundColor Cyan
Write-Host '  汇总' -ForegroundColor Cyan
Write-Host ('=' * 70) -ForegroundColor Cyan
$results | Format-Table -AutoSize | Out-Host

$failed = @($results | Where-Object { $_.Status -ne 'OK' }).Count
if ($failed -gt 0) { Write-Host "$failed 项失败" -ForegroundColor Red; exit 1 }
Write-Host '全部通过' -ForegroundColor Green
exit 0
