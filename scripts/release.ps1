<#
.SYNOPSIS
    把 dist/ 里的 SDK 包发布到 GitHub Release（推送提交+tag → 建 release → 上传产物）。

.DESCRIPTION
    为什么要有这个脚本：0.1.0 的四个包是手工上传的，流程没有留下任何记录，
    换台机器就复现不出来。这里把"发布"固定成一条命令。

    它会做四件事：
      1. 从 CMakeLists.txt 读版本号（project(... VERSION x.y.z)）；
      2. 把 main 和 v<版本> 推到 GitHub（用 GH_TOKEN；git 走 OpenSSL 后端，
         因为本机配了 http.proxy=127.0.0.1:7897，schannel 走代理握手会失败）；
      3. 用 CHANGELOG.md 里对应版本的章节作为 release notes 建 release；
      4. 逐个上传 dist/ 下匹配本版本的文件，上传后校验字节数，已存在则跳过。

    token 只从环境变量 GH_TOKEN 读，**绝不写入任何文件**。
    PAT 需要 `Contents: Read and write` + `Workflows: Read and write`
    （后者是硬要求：仓库里有 .github/workflows/，缺这个权限 GitHub 会拒绝推送）。

.EXAMPLE
    $env:GH_TOKEN = '<fine-grained PAT>'
    .\scripts\release.ps1

.EXAMPLE
    # 只看会做什么，不真的调用 API / 不推送
    .\scripts\release.ps1 -DryRun

.EXAMPLE
    # 代码已经手工推过了，只发 release
    .\scripts\release.ps1 -SkipPush
#>
[CmdletBinding()]
param(
    [string] $Tag,
    [string] $DistDir = 'dist',
    [string] $Repo    = 'dowdyboy/cpp_my_cpp_libs',
    [string] $NotesFile,
    [switch] $SkipPush,
    [switch] $DryRun
)

# PS 5.1 的坑：$ErrorActionPreference='Stop' 会把原生命令（git/curl）写到 stderr 的正常输出
# 当成终止错误（NativeCommandError）。所以用 Continue，错误一律靠 $LASTEXITCODE 和 try/catch 判。
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Fail($msg, $code = 1) { Write-Host "错误: $msg" -ForegroundColor Red; exit $code }
function Info($msg) { Write-Host "  $msg" }
function Step($msg) { Write-Host "`n== $msg ==" -ForegroundColor Cyan }

# --------------------------------------------------------------------------- 0. 前置
$token = $env:GH_TOKEN
if (-not $DryRun -and [string]::IsNullOrWhiteSpace($token)) {
    Fail "请先设置环境变量 GH_TOKEN（不要把 token 写进仓库里的任何文件）"
}

$cmake = Get-Content CMakeLists.txt -Encoding UTF8 -Raw
if ($cmake -notmatch 'project\s*\(\s*my_cpp_libs\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    Fail "没从 CMakeLists.txt 里读到 project(... VERSION x.y.z)"
}
$version = $Matches[1]
if (-not $Tag) { $Tag = "v$version" }
Info "版本: $version  tag: $Tag  repo: $Repo"

# --------------------------------------------------------------------------- 1. release notes
if (-not $NotesFile) {
    $text = Get-Content CHANGELOG.md -Encoding UTF8 -Raw
    # 抓 "## <version>..." 到下一个 "## " 之前
    $pattern = "(?ms)^##\s+$([regex]::Escape($version))[^\n]*\n(.*?)(?=^##\s|\z)"
    if ($text -match $pattern) {
        $notes = $Matches[1].Trim()
    } else {
        $notes = "见 CHANGELOG.md"
        Write-Host "  [warn] CHANGELOG.md 里没找到 $version 的章节，release notes 用兜底文案" -ForegroundColor Yellow
    }
    $notes += "`n`n---`n本版本的 SDK 包由 ``cmake/Packaging.cmake``（CPack）生成，包含全部三方许可证；" +
              "重建方法与校验命令见 ``docs/03-构建与交叉编译.md`` §4.4。"
} else {
    if (-not (Test-Path $NotesFile)) { Fail "notes 文件不存在: $NotesFile" }
    $notes = Get-Content $NotesFile -Encoding UTF8 -Raw
}
Info ("release notes 长度: {0} 字符" -f $notes.Length)

# --------------------------------------------------------------------------- 2. 待上传产物
$assets = @()
if (Test-Path $DistDir) {
    $assets = Get-ChildItem $DistDir -File | Where-Object {
        $_.Name -match [regex]::Escape($version) -and $_.Name -match '\.(zip|tar\.gz)$'
    } | Sort-Object Name
}
if ($assets.Count -eq 0) { Write-Host "  [warn] $DistDir 下没有匹配 $version 的包（先跑 cpack 生成）" -ForegroundColor Yellow }
else { $assets | ForEach-Object { Info ("{0,-48} {1,6} MB" -f $_.Name, [math]::Round($_.Length / 1MB, 1)) } }

# --------------------------------------------------------------------------- 3. 推送
$gitUrl = "https://x-access-token:$token@github.com/$Repo.git"
$pushOk = $false
if (-not $SkipPush) {
    Step "推送 main 与 tag $Tag"
    if ($DryRun) {
        Info "[dry-run] git push main + refs/tags/$Tag"
        $pushOk = $true
    } else {
        # 远端已有这个 tag 就跳过（幂等：重复跑不会报错）
        $remote = git -c http.sslBackend=openssl ls-remote $gitUrl "refs/tags/$Tag" 2>&1
        if ($remote -match [regex]::Escape("refs/tags/$Tag")) {
            Info "远端已有 $Tag，跳过推送"
            $pushOk = $true
        } else {
            $out = & git -c credential.helper= -c http.sslBackend=openssl push $gitUrl "refs/heads/main:refs/heads/main" "refs/tags/$Tag" 2>&1
            $gitExit = $LASTEXITCODE
            $out = $out | ForEach-Object { $_ -replace [regex]::Escape($token), '***' }
            $out | ForEach-Object { Info $_ }
            if ($gitExit -eq 0) {
                $pushOk = $true
            } elseif (($out -join "`n") -match 'workflow') {
                Write-Host ""
                Write-Host "推送被 GitHub 拒绝：这个 PAT 没有 workflow 权限。" -ForegroundColor Yellow
                Write-Host "  原因：本次提交里包含 .github/workflows/ci.yml 的改动，" -ForegroundColor Yellow
                Write-Host "        而细粒度 PAT 需要 'Workflows: Read and write' 才能推送工作流文件。" -ForegroundColor Yellow
                Write-Host "  修法（二选一）：" -ForegroundColor Yellow
                Write-Host "    a) 到 Settings → Developer settings → Fine-grained tokens，给这个 token 加上" -ForegroundColor Yellow
                Write-Host "       Repository permissions → Workflows: Read and write（Contents 也要 Read and write）" -ForegroundColor Yellow
                Write-Host "    b) 用带 workflow scope 的 classic token 重新执行本脚本" -ForegroundColor Yellow
                exit 3
            } else {
                Fail "git push 失败（exit=$gitExit，见上面的输出）"
            }
        }
    }
} else {
    Info "跳过推送（-SkipPush）"
    $pushOk = $true
}

# --------------------------------------------------------------------------- 4. 建 release
$apiBase = "https://api.github.com/repos/$Repo"
$headers = @{
    Authorization          = "Bearer $token"
    'User-Agent'           = 'dsh-agent'
    'X-GitHub-Api-Version' = '2022-11-28'
    Accept                 = 'application/vnd.github+json'
}

$release = $null
Step "查找 / 创建 release $Tag"
if ($DryRun) {
    Info "[dry-run] GET $apiBase/releases/tags/$Tag → 不存在则 POST"
} else {
    try {
        $release = Invoke-RestMethod -Uri "$apiBase/releases/tags/$Tag" -Headers $headers -TimeoutSec 30
        Info "release 已存在（id=$($release.id), assets=$($release.assets.Count)）"
    } catch {
        $status = $_.Exception.Response.StatusCode.value__
        if ($status -ne 404) { Fail "查询 release 失败: $($_.Exception.Message)" }
        Info "release 不存在，创建中…"
        $body = @{
            tag_name         = $Tag
            name             = "my_cpp_libs $version"
            body             = $notes
            draft            = $false
            prerelease       = $false
            target_commitish = 'main'
        } | ConvertTo-Json -Depth 5
        try {
            $release = Invoke-RestMethod -Uri "$apiBase/releases" -Method Post -Headers $headers `
                -Body ([System.Text.Encoding]::UTF8.GetBytes($body)) -ContentType 'application/json' -TimeoutSec 60
            Info "已创建 release id=$($release.id)"
        } catch {
            Fail "创建 release 失败: $($_.Exception.Message)"
        }
    }
}

# --------------------------------------------------------------------------- 5. 上传产物
if (($release -or $DryRun) -and $assets.Count -gt 0) {
    Step "上传产物到 release"
    $existing = @{}
    foreach ($a in $release.assets) { $existing[$a.name] = $a.size }

    if ($null -eq (Get-Command curl.exe -ErrorAction SilentlyContinue)) {
        Fail "需要 curl.exe 上传大文件（Windows 10+ 自带）"
    }

    foreach ($f in $assets) {
        if ($existing.ContainsKey($f.Name)) {
            $remoteSize = [int64]$existing[$f.Name]
            if ($remoteSize -eq $f.Length) {
                Info "跳过（已存在且大小一致）: $($f.Name)"
                continue
            }
            Write-Host "  已存在但大小不同（$remoteSize vs $($f.Length)），先删再传: $($f.Name)" -ForegroundColor Yellow
            $delName = $f.Name
            $assetId = ($release.assets | Where-Object { $_.name -eq $delName } | Select-Object -First 1).id
            if ($assetId -and -not $DryRun) {
                Invoke-RestMethod -Uri "$apiBase/releases/assets/$assetId" -Method Delete -Headers $headers -TimeoutSec 30 | Out-Null
            }
        }
        if ($DryRun) { Info "[dry-run] 上传 $($f.Name)"; continue }

        $url = "https://uploads.github.com/repos/$Repo/releases/$($release.id)/assets?name=$($f.Name)"
        $json = & curl.exe -sS -X POST `
            -H "Authorization: Bearer $token" `
            -H 'Content-Type: application/octet-stream' `
            -H 'User-Agent: dsh-agent' `
            --data-binary "@$($f.FullName)" `
            --max-time 1800 `
            $url 2>&1
        if ($LASTEXITCODE -ne 0) { Fail "上传失败（curl exit $LASTEXITCODE）: $json" }

        try { $resp = $json | ConvertFrom-Json } catch { Fail "上传返回无法解析: $json" }
        if ($resp.size -ne $f.Length) {
            Fail "上传后字节数不一致: $($f.Name) 本地=$($f.Length) 远端=$($resp.size)"
        }
        Info ("上传完成 {0}  ({1:N0} 字节)" -f $f.Name, $resp.size)
    }
}

# --------------------------------------------------------------------------- 6. 收尾核对
Step "核对"
if ($DryRun) {
    Info "[dry-run] 不做最终核对"
} else {
    $final = Invoke-RestMethod -Uri "$apiBase/releases/tags/$Tag" -Headers $headers -TimeoutSec 30
    Info "release: $($final.html_url)"
    Info "tag=$($final.tag_name)  draft=$($final.draft)  assets=$($final.assets.Count)"
    foreach ($a in $final.assets) { Info ("  - {0,-48} {1,8:N0} B  下载 {2}" -f $a.name, $a.size, $a.download_count) }
    $missing = $assets | Where-Object { $n = $_.Name; -not ($final.assets | Where-Object { $_.name -eq $n }) }
    if ($missing) { Fail ("以下产物没有上传成功: " + (($missing | ForEach-Object { $_.Name }) -join ', ')) }
    Write-Host "`n发布完成: $($final.html_url)" -ForegroundColor Green
}
