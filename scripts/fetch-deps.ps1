<#
.SYNOPSIS
    下载 resources/ 里缺失的三方库压缩包（Windows 侧，等价于 scripts/fetch-deps.sh）。

.DESCRIPTION
    resources/*.zip 不进 git（体积大），clone 之后补一次即可。
    离线环境下跳过本脚本，手工把 zip 放进 resources/。

    下载优先用 curl.exe（Windows 10+ / GitHub Runner 自带）：
    Windows PowerShell 5.1 的 Invoke-WebRequest 在 TLS 1.2 上经常报
    "基础连接已经关闭"，ps7 才好使；curl 没有这个问题。

.EXAMPLE
    .\scripts\fetch-deps.ps1
    .\scripts\fetch-deps.ps1 -WithOrtWin
#>
[CmdletBinding()]
param(
    [switch] $WithOrtWin
)

$ErrorActionPreference = 'Stop'

# 万一没有 curl 还得走 Invoke-WebRequest：显式开 TLS 1.2
try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
} catch { }

$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'resources'
New-Item -ItemType Directory -Force $dest | Out-Null

$curl = (Get-Command curl.exe -ErrorAction SilentlyContinue).Source

function Get-Url([string] $url, [string] $outFile) {
    if ($curl) {
        & $curl -fsSL --retry 3 --retry-delay 2 -o $outFile $url
        if ($LASTEXITCODE -ne 0) { throw "下载失败(exit $LASTEXITCODE): $url" }
    }
    else {
        Invoke-WebRequest -Uri $url -OutFile $outFile -UseBasicParsing
    }
}

function Fetch([string] $name, [string] $url) {
    $target = Join-Path $dest $name
    if ((Test-Path $target) -and ((Get-Item $target).Length -gt 0)) {
        Write-Host "[skip] $name（已存在）" -ForegroundColor DarkGray
        return
    }
    Write-Host "[get ] $name"
    # 先写 .part 再 rename：断流时不会留下一个"看起来完整"的坏压缩包
    $part = "$target.part"
    Get-Url $url $part
    Move-Item -Force $part $target
}

# 读两个/一个头文件、校验 SHA256、打成本工程约定的 zip。
# 上游没有 tag 的库（stb / pocketfft）都走这里；换版本 = 改 PIN / SHA / 文件名三处。
function Fetch-HeaderZip([string] $zipName, [string] $pin, [hashtable] $files, [string] $rawBase) {
    $target = Join-Path $dest $zipName
    if ((Test-Path $target) -and ((Get-Item $target).Length -gt 0)) {
        Write-Host "[skip] $zipName（已存在）" -ForegroundColor DarkGray
        return
    }
    Write-Host "[get ] $zipName（@ $pin）"
    $tmp = Join-Path $env:TEMP ("hdr-" + [guid]::NewGuid().ToString('N'))
    $tmpZip = Join-Path $env:TEMP ("hdr-" + [guid]::NewGuid().ToString('N') + ".zip")
    New-Item -ItemType Directory -Force $tmp | Out-Null
    try {
        foreach ($name in $files.Keys) {
            $file = Join-Path $tmp $name
            Get-Url "$rawBase/$pin/$name" $file
            $actual = (Get-FileHash $file -Algorithm SHA256).Hash
            if ($actual -ne $files[$name]) {
                throw "$name 的 SHA256 不匹配：期望 $($files[$name])，实际 $actual（上游内容变了？）"
            }
        }
        Compress-Archive -Path (Join-Path $tmp '*') -DestinationPath $tmpZip -CompressionLevel Optimal
        Move-Item -Force $tmpZip $target
    }
    finally {
        Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
        Remove-Item -Force $tmpZip -ErrorAction SilentlyContinue
    }
}

# ---------------------------------------------------------------- 有正式发布包的库
Fetch 'eigen-5.0.0.zip'   'https://gitlab.com/libeigen/eigen/-/archive/5.0.0/eigen-5.0.0.zip'
Fetch 'spdlog-1.17.0.zip' 'https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.zip'
Fetch 'json-include.zip'  'https://github.com/nlohmann/json/releases/download/v3.12.0/include.zip'

if ($WithOrtWin) {
    Fetch 'onnxruntime-win-x64-1.23.2.zip' `
          'https://github.com/microsoft/onnxruntime/releases/download/v1.23.2/onnxruntime-win-x64-1.23.2.zip'
}

# ---------------------------------------------------------------- 单头文件库（按 commit 固定）
Fetch-HeaderZip 'stb-image-2.32.zip' '9600522de53973336fa033a289858a4354cbb04c' ([ordered]@{
    'stb_image.h'       = 'DEB4F18C9CA21B751A1323019031C94C0BF5CA1D2929EF5913CC56F00AAB6E91'
    'stb_image_write.h' = 'A8E7F77F0CCFDC9BAC09A71268B131931FF81AAFCA82C393AEB6AABA10D9E7CA'
}) 'https://raw.githubusercontent.com/nothings/stb'

Fetch-HeaderZip 'httplib-0.66.0.zip' 'cdcd2ac6e6d5548b5a9130fb39ab76d2c6c614b0' ([ordered]@{
    'httplib.h' = '51DDD1482C67ACB31F3064E48329AADE14ACC7DD0FAB31EE5397193697EA1C35'
    'LICENSE'   = '4B45CBE16D7B71B89AE6127E26E0D90A029198CA5E958AD8E3D0B8BBED364D8B'
}) 'https://raw.githubusercontent.com/yhirose/cpp-httplib'

Fetch-HeaderZip 'pocketfft-4d865cb.zip' '4d865cba24359b05b37da4e75aee24498c3b78e7' ([ordered]@{
    'pocketfft_hdronly.h' = '518A95C552AEAEE352AA30122137CD9E135D23C91E2863D9272AAA758744C86D'
}) 'https://raw.githubusercontent.com/mreineck/pocketfft'

Write-Host ""
Write-Host "完成。解压目录: $root\third_party（首次 cmake configure 时自动生成）"
