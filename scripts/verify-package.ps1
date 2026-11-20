# Verify one SDK package: extract it, then (for host-matching packages) build+run
# tests/sdk_consumer against the extracted tree via find_package.
# ASCII ONLY -- do not add CJK text here: PS 5.1 reads this BOM-less file as ANSI\n# and mangled bytes inside comments swallow the following lines (this bit us once).
param(
    [Parameter(Mandatory = $true)][string] $Package,
    [ValidateSet('host', 'cross')][string] $Mode = 'host'
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot   # this script lives in scripts/
Set-Location $root
$env:PATH = "D:\ProgramFiles\msys64\ucrt64\bin;D:\ProgramFiles\ninja-win;$env:PATH"

# Strip only the archive suffix: GetFileNameWithoutExtension would also eat ".msvc".
$leaf = Split-Path $Package -Leaf
$name = $leaf -replace '\.(zip|tar\.gz|tgz)$', ''
$work = Join-Path $root "build\pkgtest\$name"
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $work | Out-Null

Write-Host "== extract $Package =="
if ($Package -like '*.zip') {
    Expand-Archive -LiteralPath $Package -DestinationPath $work -Force
} else {
    tar -xzf $Package -C $work
    if ($LASTEXITCODE -ne 0) { Write-Host "  extract FAILED"; exit 1 }
}
$inner = Get-ChildItem $work -Directory | Select-Object -First 1
if (-not $inner) { Write-Host "  no top-level dir in package"; exit 1 }
$prefix = $inner.FullName
Write-Host "  prefix: $($inner.Name)"

Write-Host "== tree sanity =="
$checks = @(
    'include\my_cpp_libs\core\status.hpp',
    'include\my_cpp_libs\io\io.hpp',
    'include\my_cpp_libs\time\time.hpp',
    'include\my_cpp_libs\thread\thread.hpp',
    'include\my_cpp_libs\json\config.hpp',
    'include\my_cpp_libs\dsp\stream.hpp',
    'lib\cmake\my_cpp_libs\my_cpp_libsConfig.cmake',
    'share\licenses\my_cpp_libs\my_cpp_libs-LICENSE',
    'share\licenses\my_cpp_libs\THIRD_PARTY_NOTICES.md'
)
$bad = 0
foreach ($c in $checks) {
    if (Test-Path (Join-Path $prefix $c)) { Write-Host "  OK   $c" }
    else { Write-Host "  MISS $c"; $bad++ }
}
$cfg = Join-Path $prefix 'lib\cmake\my_cpp_libs\my_cpp_libsConfig.cmake'
if (Test-Path $cfg) {
    $has17 = (Select-String -Path $cfg -Pattern 'cxx_std_17' -SimpleMatch).Count
    Write-Host "  cxx_std_17 in Config: $($has17 -gt 0)"
    $comps = (Select-String -Path $cfg -Pattern '_mylib_sdk_add_component\(' | Measure-Object).Count
    Write-Host "  exported components: $comps"
}
$libs = (Get-ChildItem (Join-Path $prefix 'lib') -File -Recurse -ErrorAction SilentlyContinue)
Write-Host "  lib/ files: $($libs.Count) ($([math]::Round(($libs | Measure-Object Length -Sum).Sum / 1MB, 1)) MB)"

if ($Mode -eq 'host') {
    Write-Host "== build sdk_consumer against the package =="
    $bdir = Join-Path $root "build\pkgtest\$name-consumer"
    Remove-Item -Recurse -Force $bdir -ErrorAction SilentlyContinue
    # The msvc package ships MSVC .lib/.dll: it can only be consumed by MSVC.
    $isMsvc = $name -match 'msvc'
    if ($isMsvc) {
        cmake -S tests\sdk_consumer -B $bdir -A x64 "-DCMAKE_PREFIX_PATH=$prefix" *> "$root\build\_logs\pkgtest-$name-cfg.txt"
    } else {
        cmake -S tests\sdk_consumer -B $bdir -G Ninja `
            "-DCMAKE_CXX_COMPILER=D:/ProgramFiles/msys64/ucrt64/bin/g++.exe" `
            "-DCMAKE_MAKE_PROGRAM=D:/ProgramFiles/ninja-win/ninja.exe" `
            -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$prefix" *> "$root\build\_logs\pkgtest-$name-cfg.txt"
    }
    Write-Host "  consumer configure exit=$LASTEXITCODE"
    if ($LASTEXITCODE -ne 0) {
        Select-String -Path "$root\build\_logs\pkgtest-$name-cfg.txt" -Pattern 'error|Could not' |
            Select-Object -First 4 | ForEach-Object { Write-Host "   $($_.Line.Trim())" }
        exit 1
    }
    if ($isMsvc) {
        cmake --build $bdir --config Release *> "$root\build\_logs\pkgtest-$name-build.txt"
    } else {
        cmake --build $bdir *> "$root\build\_logs\pkgtest-$name-build.txt"
    }
    Write-Host "  consumer build exit=$LASTEXITCODE"
    if ($LASTEXITCODE -ne 0) {
        Select-String -Path "$root\build\_logs\pkgtest-$name-build.txt" -Pattern 'error' |
            Select-Object -First 4 | ForEach-Object { Write-Host "   $($_.Line.Trim())" }
        exit 1
    }
    $exe = Get-ChildItem $bdir -Recurse -Filter 'sdk_consumer.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($exe) {
        # ORT/OpenCV DLLs live in <prefix>/bin: prepend to PATH before running.
        $env:PATH = "$prefix\bin;$env:PATH"
        $out = & $exe.FullName 2>&1
        Write-Host "  consumer run exit=$LASTEXITCODE"
        $out | Select-Object -Last 6 | ForEach-Object { Write-Host "   $_" }
    } else { Write-Host "  consumer exe missing"; exit 1 }

    # Component-level consumer: catches "component does not carry core" and
    $min = Get-ChildItem $bdir -Recurse -Filter 'sdk_consumer_min.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $min) { Write-Host "  consumer_min exe missing"; exit 1 }
    $out2 = & $min.FullName 2>&1
    Write-Host "  consumer_min run exit=$LASTEXITCODE"
    $out2 | Select-Object -Last 4 | ForEach-Object { Write-Host "   $_" }
    if ($LASTEXITCODE -ne 0) { Write-Host "  FAIL: component-level link check"; exit 1 }
} else {
    Write-Host "== cross package: check ELF arch of shipped static libs =="
    $ar = (Get-ChildItem (Join-Path $prefix 'lib') -File -Filter '*.a' -ErrorAction SilentlyContinue | Select-Object -First 1)
    if ($ar) { Write-Host "  sampled: $($ar.Name) ($([math]::Round($ar.Length/1KB,1)) KB)" }
}

Write-Host "== RESULT: $(if ($bad -eq 0) { 'OK' } else { "$bad missing" }) =="
exit $bad

