<#
.SYNOPSIS
    下载 resources/ 里缺失的三方库压缩包（Windows 侧，等价于 scripts/fetch-deps.sh）。

.DESCRIPTION
    resources/*.zip 不进 git（体积大），clone 之后补一次即可。
    离线环境下跳过本脚本，手工把 zip 放进 resources/。

.EXAMPLE
    .\scripts\fetch-deps.ps1
    .\scripts\fetch-deps.ps1 -WithOrtWin
#>
[CmdletBinding()]
param(
    [switch] $WithOrtWin
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'resources'
New-Item -ItemType Directory -Force $dest | Out-Null

function Fetch([string] $name, [string] $url) {
    $target = Join-Path $dest $name
    if ((Test-Path $target) -and ((Get-Item $target).Length -gt 0)) {
        Write-Host "[skip] $name（已存在）" -ForegroundColor DarkGray
        return
    }
    Write-Host "[get ] $name"
    $part = "$target.part"
    Invoke-WebRequest -Uri $url -OutFile $part -UseBasicParsing
    Move-Item -Force $part $target
}

Fetch 'eigen-5.0.0.zip'   'https://gitlab.com/libeigen/eigen/-/archive/5.0.0/eigen-5.0.0.zip'
Fetch 'spdlog-1.17.0.zip' 'https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.zip'
Fetch 'json-include.zip'  'https://github.com/nlohmann/json/releases/download/v3.12.0/include.zip'

if ($WithOrtWin) {
    Fetch 'onnxruntime-win-x64-1.23.2.zip' `
          'https://github.com/microsoft/onnxruntime/releases/download/v1.23.2/onnxruntime-win-x64-1.23.2.zip'
}

Write-Host ""
Write-Host "完成。解压目录: $root\third_party（首次 cmake configure 时自动生成）"
