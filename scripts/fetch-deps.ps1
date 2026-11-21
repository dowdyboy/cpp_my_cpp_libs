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
    [switch] $WithOrtWin,
    # CI / 发布用：ORT 下载失败必须让任务失败，而不是让 ml 静默 skip。
    [switch] $RequireOrtWin,
    # 上游 microsoft/onnxruntime 的 GitHub Releases 在 2026-11-17 起不可用（仓库/资产 404）。
    # 默认改用本仓库固定版本的 release 资产（内容与官方 zip 一致，SHA256 见下）；
    # CI 仍可用仓库变量 MYLIB_ORT_WIN_URL / -OrtWinUrl 覆盖成别的镜像。
    [string] $OrtWinUrl = 'https://github.com/dowdyboy/cpp_my_cpp_libs/releases/download/deps-ort-1.23.2/onnxruntime-win-x64-1.23.2.zip'
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

if ($WithOrtWin -or $RequireOrtWin) {
    # 注意：官方直链 2026-11-17 起 404（上游 release 不可用），默认走本仓库的固定资产。
    # 本地默认容忍失败；CI/发布传 -RequireOrtWin，拿不到 ORT 必须让任务失败，
    # 否则 ml 的 golden 用例会静默 skip，而流水线仍然全绿。
    try {
        $ortName = Split-Path $OrtWinUrl -Leaf
        Fetch $ortName $OrtWinUrl
        # 固定版本 + 固定摘要：mirror/缓存拿到的内容必须与官方 1.23.2 完全一致。
        $ortSha = '0b38df9af21834e41e73d602d90db5cb06dbd1ca618948b8f1d66d607ac9f3cd'
        $ortPath = Join-Path (Join-Path $PSScriptRoot '..\resources') $ortName
        if (Test-Path $ortPath) {
            $actual = (Get-FileHash $ortPath -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($actual -ne $ortSha) {
                throw "$ortName 的 SHA256 不匹配：期望 $ortSha，实际 $actual（镜像内容不对？）"
            }
        }
    } catch {
        if ($RequireOrtWin) {
            throw "ONNX Runtime 下载失败（$($_.Exception.Message)）。CI/发布要求必须有 ORT；" +
                  "可用 -OrtWinUrl 指定镜像或修好下载地址。"
        }
        Write-Warning "ONNX Runtime 下载失败（$($_.Exception.Message)）—— infer 会退回 stub，ml 用例将跳过"
        if ($env:GITHUB_ACTIONS) { Write-Host "::warning::ONNX Runtime 预编译包下载失败：ml 用例本轮将跳过" }
    }
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
# 显式 exit 0：脚本里最后一条原生命令（curl）失败时，powershell -File 会把它的退出码
# 当成整个脚本的退出码带出去 —— 那会让"已捕获的失败"仍然把 CI 判红。
# 真正的致命错误走 throw，会在此之前就非零退出。
exit 0