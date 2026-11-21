# Rebuild all four SDK packages at the current version via CPack.
# ASCII only (PS 5.1 reads BOM-less files as ANSI).
param(
    # Skip the unpack/verify step (packaging only; verify runs by default)
    [switch] $SkipVerify,
    # Allow packaging from a dirty working tree (NOT for releases; the resulting
    # package will contain "commit-dirty" in BUILD-INFO and cannot be rebuilt
    # from the tag).
    [switch] $AllowDirty
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot   # this script lives in scripts/
Set-Location $root
$env:PATH = "D:\ProgramFiles\msys64\ucrt64\bin;D:\ProgramFiles\ninja-win;$env:PATH"
$gxx = "D:/ProgramFiles/msys64/ucrt64/bin/g++.exe"
$ninja = "D:/ProgramFiles/ninja-win/ninja.exe"

# A clean clone has no build/ directory (it is gitignored), so create the log
# directory before the first Out-File. Without this the script dies with
# DirectoryNotFoundException on any fresh checkout.
New-Item -ItemType Directory -Force -Path (Join-Path $root 'build\_logs') | Out-Null

$log = "$root\build\_logs\pkg-all.txt"
"" | Out-File -Encoding utf8 $log
function Say($m) { Write-Host $m; $m | Out-File -Encoding utf8 -Append $log }
function Assert-Exit($what) {
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE); see build\_logs" }
}

# Release packages must be rebuildable from the tag. Refuse to package a dirty
# tree unless the caller explicitly opts in (local experiments).
$git = Get-Command git -ErrorAction SilentlyContinue
if (-not $git) {
    Write-Warning "git not found in PATH; cannot verify the tree is clean"
} else {
    git -C $root diff-index --quiet HEAD --
    if ($LASTEXITCODE -ne 0) {
        if (-not $AllowDirty) {
            throw ("working tree is dirty (git diff-index exit $LASTEXITCODE). " +
                   "Commit or stash first, or pass -AllowDirty for a throwaway package.")
        }
        Write-Warning "packaging a dirty tree (-AllowDirty): BUILD-INFO will say '-dirty'"
    }
}

# NOTE: the SDK package does NOT ship ncnn. Its static lib pulls openmp/vulkan
# transitive deps, and reproducing that in the package Config is how you get a
# second class of "undefined symbol" bugs. So build packages with NCNN=OFF, which
# keeps libmy_cpp_libs_infer free of ncnn references. (0.1.7 and earlier had it ON,
# so any consumer linking my_cpp_libs::infer failed to link.) Want ncnn? Build from source.

# ---- 1. MinGW basic ----
Say "== package: win-x64-mingw-basic =="
cmake -S . -B build\pkg-mingw -G Ninja "-DCMAKE_CXX_COMPILER=$gxx" "-DCMAKE_MAKE_PROGRAM=$ninja" `
  -DCMAKE_BUILD_TYPE=Release -DMYLIB_BUILD_EXAMPLES=OFF -DMYLIB_BUILD_TESTS=OFF `
  -DMYLIB_BUILD_TOOLS=OFF -DMYLIB_ENABLE_NCNN=OFF `
  -DMYLIB_PACKAGE_SUFFIX=win-x64-mingw-basic *> "$root\build\_logs\pkg-mingw-cfg.txt"
Say "   configure=$LASTEXITCODE"
Assert-Exit "configure"
cmake --build build\pkg-mingw *> "$root\build\_logs\pkg-mingw-build.txt"
Say "   build=$LASTEXITCODE"
Assert-Exit "build"
cpack --config build\pkg-mingw\CPackConfig.cmake -B dist *> "$root\build\_logs\pkg-mingw-cpack.txt"
Say "   cpack=$LASTEXITCODE  $(Select-String -Path "$root\build\_logs\pkg-mingw-cpack.txt" -Pattern 'CPack: - package' | ForEach-Object { $_.Line.Trim() })"
Assert-Exit "cpack"

# ---- 2. MSVC full ----
Say "== package: win-x64-msvc =="
cmake -S . -B build\pkg-msvc -A x64 -DMYLIB_BUILD_EXAMPLES=OFF -DMYLIB_BUILD_TESTS=OFF `
  -DMYLIB_BUILD_TOOLS=OFF -DMYLIB_ENABLE_CV=ON -DMYLIB_ENABLE_NCNN=OFF `
  -DMYLIB_PACKAGE_SUFFIX=win-x64-msvc *> "$root\build\_logs\pkg-msvc-cfg.txt"
Say "   configure=$LASTEXITCODE"
Assert-Exit "configure"
cmake --build build\pkg-msvc --config Release *> "$root\build\_logs\pkg-msvc-build.txt"
Say "   build=$LASTEXITCODE"
Assert-Exit "build"
cpack --config build\pkg-msvc\CPackConfig.cmake -C Release -B dist *> "$root\build\_logs\pkg-msvc-cpack.txt"
Say "   cpack=$LASTEXITCODE  $(Select-String -Path "$root\build\_logs\pkg-msvc-cpack.txt" -Pattern 'CPack: - package' | ForEach-Object { $_.Line.Trim() })"
Assert-Exit "cpack"

# ---- 3. armv7 cross ----
Say "== package: linux-armv7-glibc239 =="
$env:PATH = "D:\ProgramData\gcc-arm-linux-gnueabi\bin;$env:PATH"
$tc7 = ((Resolve-Path cmake\toolchains\linux-armv7-zynq.cmake -ErrorAction Stop).Path -replace '\\', '/')
cmake -S . -B build\pkg-armv7 -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_TOOLCHAIN_FILE=$tc7" `
  -DCMAKE_BUILD_TYPE=Release -DMYLIB_BUILD_EXAMPLES=OFF -DMYLIB_BUILD_TESTS=OFF `
  -DMYLIB_BUILD_TOOLS=OFF -DMYLIB_ENABLE_CV=ON -DMYLIB_ENABLE_NCNN=OFF `
  -DMYLIB_PACKAGE_SUFFIX=linux-armv7-glibc239 *> "$root\build\_logs\pkg-armv7-cfg.txt"
Say "   configure=$LASTEXITCODE"
Assert-Exit "configure"
cmake --build build\pkg-armv7 *> "$root\build\_logs\pkg-armv7-build.txt"
Say "   build=$LASTEXITCODE"
Assert-Exit "build"
cpack --config build\pkg-armv7\CPackConfig.cmake -B dist *> "$root\build\_logs\pkg-armv7-cpack.txt"
Say "   cpack=$LASTEXITCODE  $(Select-String -Path "$root\build\_logs\pkg-armv7-cpack.txt" -Pattern 'CPack: - package' | ForEach-Object { $_.Line.Trim() })"
Assert-Exit "cpack"

# ---- 4. aarch64 cross ----
Say "== package: linux-aarch64 =="
$env:PATH = "D:\ProgramData\arm-gnu-toolchain-14.2.rel1-mingw-w64-x86_64-aarch64-none-linux-gnu\bin;$env:PATH"
$tca = ((Resolve-Path cmake\toolchains\linux-aarch64.cmake -ErrorAction Stop).Path -replace '\\', '/')
cmake -S . -B build\pkg-aarch64 -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_TOOLCHAIN_FILE=$tca" `
  -DCMAKE_BUILD_TYPE=Release -DMYLIB_BUILD_EXAMPLES=OFF -DMYLIB_BUILD_TESTS=OFF `
  -DMYLIB_BUILD_TOOLS=OFF -DMYLIB_ENABLE_CV=ON -DMYLIB_ENABLE_NCNN=OFF `
  -DMYLIB_PACKAGE_SUFFIX=linux-aarch64 *> "$root\build\_logs\pkg-aarch64-cfg.txt"
Say "   configure=$LASTEXITCODE"
Assert-Exit "configure"
cmake --build build\pkg-aarch64 *> "$root\build\_logs\pkg-aarch64-build.txt"
Say "   build=$LASTEXITCODE"
Assert-Exit "build"
cpack --config build\pkg-aarch64\CPackConfig.cmake -B dist *> "$root\build\_logs\pkg-aarch64-cpack.txt"
Say "   cpack=$LASTEXITCODE  $(Select-String -Path "$root\build\_logs\pkg-aarch64-cpack.txt" -Pattern 'CPack: - package' | ForEach-Object { $_.Line.Trim() })"
Assert-Exit "cpack"

Say ""
Say "== dist/ =="
Get-ChildItem dist -File | Sort-Object Name | ForEach-Object { Say ("   {0,-48} {1,6} MB" -f $_.Name, [math]::Round($_.Length / 1MB, 1)) }

# ---- 5. unpack and verify every package (README promises one command) ----
$specs = @(
    @{ Suffix = 'win-x64-mingw-basic'; Ext = 'zip';    Mode = 'host'  },
    @{ Suffix = 'win-x64-msvc';        Ext = 'zip';    Mode = 'host'  },
    @{ Suffix = 'linux-armv7-glibc239'; Ext = 'tar.gz'; Mode = 'cross' },
    @{ Suffix = 'linux-aarch64';       Ext = 'tar.gz'; Mode = 'cross' }
)
foreach ($s in $specs) {
    $pkg = Get-ChildItem dist -File -Filter "*$($s.Suffix).$($s.Ext)" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $pkg) { throw "package missing: *$($s.Suffix).$($s.Ext)" }
    if (-not $SkipVerify) {
        Say "== verify: $($pkg.Name) =="
        $verifyArgs = @{ Package = $pkg.FullName; Mode = $s.Mode }
        if (-not $AllowDirty) { $verifyArgs['StrictCommit'] = $true }
        & "$PSScriptRoot\verify-package.ps1" @verifyArgs
        if ($LASTEXITCODE -ne 0) { throw "verify-package failed: $($pkg.Name)" }
    }
}
