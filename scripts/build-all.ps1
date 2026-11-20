<#
.SYNOPSIS
    一次性构建/测试 my_cpp_libs 的全部目标平台。

.DESCRIPTION
    依次执行 CMakePresets.json 里的 6 个预设（多一个 linux-armv7-static）：
        win-msvc / win-mingw / linux-x64 / linux-armv7 / linux-armv7-static / linux-aarch64
    宿主平台（Windows）的构建会顺带跑单元测试；交叉编译只验证能编过、能链接。

.EXAMPLE
    .\scripts\build-all.ps1
    .\scripts\build-all.ps1 -Only win-mingw,linux-armv7
    .\scripts\build-all.ps1 -Clean
#>
[CmdletBinding()]
param(
    [string[]] $Only,
    [switch]   $Clean,
    [switch]   $SkipTests
)

# 注意：不能用 'Stop'。PowerShell 5.1 会把原生命令写到 stderr 的任何内容
# （比如 CMake 的一句 Warning）当成终止性错误，导致构建明明成功却报失败。
# 失败判定一律看 $LASTEXITCODE。
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# 让 Ninja / MinGW 可直接调用
$env:PATH = "D:\ProgramFiles\ninja-win;D:\ProgramFiles\msys64\ucrt64\bin;$env:PATH"

$presets = @(
    @{ Name = 'win-msvc';     Kind = 'host';  Desc = 'Windows x64 / MSVC (VS 2026)' },
    @{ Name = 'win-mingw';    Kind = 'host';  Desc = 'Windows x64 / MinGW-w64 GCC 15.2' },
    @{ Name = 'linux-x64';    Kind = 'cross'; Desc = 'Linux x86_64 / GCC 9.3 (glibc 2.31)' },
    @{ Name = 'linux-armv7';  Kind = 'cross'; Desc = 'Linux armv7-A HF / 2024 SDK (glibc 2.39，仅供编译验证)' },
    @{ Name = 'linux-armv7-static'; Kind = 'cross'; Desc = 'Linux armv7-A 全静态（可上板）' },
    @{ Name = 'linux-aarch64';Kind = 'cross'; Desc = 'Linux aarch64 / ARM GNU 14.2' }
)

if ($Only) {
    $presets = $presets | Where-Object { $Only -contains $_.Name }
}

$results = @()

foreach ($p in $presets) {
    $name = $p.Name
    Write-Host ""
    Write-Host ("=" * 70) -ForegroundColor Cyan
    Write-Host "  $name  —  $($p.Desc)" -ForegroundColor Cyan
    Write-Host ("=" * 70) -ForegroundColor Cyan

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $status = 'OK'
    $detail = ''

    try {
        if ($Clean) {
            Remove-Item -Recurse -Force "build/$name" -ErrorAction SilentlyContinue
        }
        cmake --preset $name 2>&1 | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "configure 失败 (exit $LASTEXITCODE)" }

        cmake --build --preset $name 2>&1 | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "build 失败 (exit $LASTEXITCODE)" }

        if ($p.Kind -eq 'host' -and -not $SkipTests) {
            ctest --preset $name 2>&1 | Out-Host
            if ($LASTEXITCODE -ne 0) { throw "测试失败 (exit $LASTEXITCODE)" }
        } elseif ($p.Kind -eq 'cross') {
            $detail = '仅编译/链接'
        }
    } catch {
        $status = 'FAIL'
        $detail = $_.Exception.Message
    }

    $sw.Stop()
    $results += [pscustomobject]@{
        Target  = $name
        Status  = $status
        Seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1)
        Note    = $detail
    }
}

Write-Host ""
Write-Host ("=" * 70) -ForegroundColor Cyan
Write-Host "  汇总" -ForegroundColor Cyan
Write-Host ("=" * 70) -ForegroundColor Cyan
$results | Format-Table -AutoSize | Out-Host

$failed = @($results | Where-Object { $_.Status -ne 'OK' }).Count
if ($failed -gt 0) {
    Write-Host "$failed 个目标失败" -ForegroundColor Red
    exit 1
}
Write-Host "全部通过" -ForegroundColor Green
exit 0
