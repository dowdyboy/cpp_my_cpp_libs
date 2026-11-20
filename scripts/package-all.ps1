# Rebuild all four SDK packages at the current version via CPack.
# ASCII only (PS 5.1 reads BOM-less files as ANSI).
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot   # 本脚本在 scripts/ 下
Set-Location $root
$env:PATH = "D:\ProgramFiles\msys64\ucrt64\bin;D:\ProgramFiles\ninja-win;$env:PATH"
$gxx = "D:/ProgramFiles/msys64/ucrt64/bin/g++.exe"
$ninja = "D:/ProgramFiles/ninja-win/ninja.exe"
# 注意：SDK 包里**不打包 ncnn**（它的静态库还带 openmp/vulkan 这些传递依赖，
# 一起打进包再在 Config 里复现链接关系容易出第二类"缺符号"问题）。所以这里显式关掉，
# 让 libmy_cpp_libs_infer 不带 ncnn 符号引用 —— 0.1.7 及以前的包是 ON 的，
# 结果"链 my_cpp_libs::infer"的消费方必然 undefined。想用 ncnn 就自己从源码构建。
$log = "$root\build\_logs\pkg-all.txt"
"" | Out-File -Encoding utf8 $log
function Say($m) { Write-Host $m; $m | Out-File -Encoding utf8 -Append $log }

# ---- 1. MinGW basic ----
Say "== package: win-x64-mingw-basic =="
cmake -S . -B build\pkg-mingw -G Ninja "-DCMAKE_CXX_COMPILER=$gxx" "-DCMAKE_MAKE_PROGRAM=$ninja" `
  -DCMAKE_BUILD_TYPE=Release -DMYLIB_BUILD_EXAMPLES=OFF -DMYLIB_BUILD_TESTS=OFF `
  -DMYLIB_BUILD_TOOLS=OFF -DMYLIB_ENABLE_NCNN=OFF `
  -DMYLIB_PACKAGE_SUFFIX=win-x64-mingw-basic *> "$root\build\_logs\pkg-mingw-cfg.txt"
Say "   configure=$LASTEXITCODE"
cmake --build build\pkg-mingw *> "$root\build\_logs\pkg-mingw-build.txt"
Say "   build=$LASTEXITCODE"
cpack --config build\pkg-mingw\CPackConfig.cmake -B dist *> "$root\build\_logs\pkg-mingw-cpack.txt"
Say "   cpack=$LASTEXITCODE  $(Select-String -Path "$root\build\_logs\pkg-mingw-cpack.txt" -Pattern 'CPack: - package' | ForEach-Object { $_.Line.Trim() })"

# ---- 2. MSVC full ----
Say "== package: win-x64-msvc =="
cmake -S . -B build\pkg-msvc -A x64 -DMYLIB_BUILD_EXAMPLES=OFF -DMYLIB_BUILD_TESTS=OFF `
  -DMYLIB_BUILD_TOOLS=OFF -DMYLIB_ENABLE_CV=ON -DMYLIB_ENABLE_NCNN=OFF `
  -DMYLIB_PACKAGE_SUFFIX=win-x64-msvc *> "$root\build\_logs\pkg-msvc-cfg.txt"
Say "   configure=$LASTEXITCODE"
cmake --build build\pkg-msvc --config Release *> "$root\build\_logs\pkg-msvc-build.txt"
Say "   build=$LASTEXITCODE"
cpack --config build\pkg-msvc\CPackConfig.cmake -C Release -B dist *> "$root\build\_logs\pkg-msvc-cpack.txt"
Say "   cpack=$LASTEXITCODE  $(Select-String -Path "$root\build\_logs\pkg-msvc-cpack.txt" -Pattern 'CPack: - package' | ForEach-Object { $_.Line.Trim() })"

# ---- 3. armv7 cross ----
Say "== package: linux-armv7-glibc239 =="
$env:PATH = "D:\ProgramData\gcc-arm-linux-gnueabi\bin;$env:PATH"
$tc7 = ((Resolve-Path cmake\toolchains\linux-armv7-zynq.cmake).Path -replace '\\', '/')
cmake -S . -B build\pkg-armv7 -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_TOOLCHAIN_FILE=$tc7" `
  -DCMAKE_BUILD_TYPE=Release -DMYLIB_BUILD_EXAMPLES=OFF -DMYLIB_BUILD_TESTS=OFF `
  -DMYLIB_BUILD_TOOLS=OFF -DMYLIB_ENABLE_CV=ON -DMYLIB_ENABLE_NCNN=OFF `
  -DMYLIB_PACKAGE_SUFFIX=linux-armv7-glibc239 *> "$root\build\_logs\pkg-armv7-cfg.txt"
Say "   configure=$LASTEXITCODE"
cmake --build build\pkg-armv7 *> "$root\build\_logs\pkg-armv7-build.txt"
Say "   build=$LASTEXITCODE"
cpack --config build\pkg-armv7\CPackConfig.cmake -B dist *> "$root\build\_logs\pkg-armv7-cpack.txt"
Say "   cpack=$LASTEXITCODE  $(Select-String -Path "$root\build\_logs\pkg-armv7-cpack.txt" -Pattern 'CPack: - package' | ForEach-Object { $_.Line.Trim() })"

# ---- 4. aarch64 cross ----
Say "== package: linux-aarch64 =="
$env:PATH = "D:\ProgramData\arm-gnu-toolchain-14.2.rel1-mingw-w64-x86_64-aarch64-none-linux-gnu\bin;$env:PATH"
$tca = ((Resolve-Path cmake\toolchains\linux-aarch64.cmake).Path -replace '\\', '/')
cmake -S . -B build\pkg-aarch64 -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_TOOLCHAIN_FILE=$tca" `
  -DCMAKE_BUILD_TYPE=Release -DMYLIB_BUILD_EXAMPLES=OFF -DMYLIB_BUILD_TESTS=OFF `
  -DMYLIB_BUILD_TOOLS=OFF -DMYLIB_ENABLE_CV=ON -DMYLIB_ENABLE_NCNN=OFF `
  -DMYLIB_PACKAGE_SUFFIX=linux-aarch64 *> "$root\build\_logs\pkg-aarch64-cfg.txt"
Say "   configure=$LASTEXITCODE"
cmake --build build\pkg-aarch64 *> "$root\build\_logs\pkg-aarch64-build.txt"
Say "   build=$LASTEXITCODE"
cpack --config build\pkg-aarch64\CPackConfig.cmake -B dist *> "$root\build\_logs\pkg-aarch64-cpack.txt"
Say "   cpack=$LASTEXITCODE  $(Select-String -Path "$root\build\_logs\pkg-aarch64-cpack.txt" -Pattern 'CPack: - package' | ForEach-Object { $_.Line.Trim() })"

Say ""
Say "== dist/ =="
Get-ChildItem dist -File | Sort-Object Name | ForEach-Object { Say ("   {0,-48} {1,6} MB" -f $_.Name, [math]::Round($_.Length / 1MB, 1)) }
