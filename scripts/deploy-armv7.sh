#!/usr/bin/env bash
# =============================================================================
#  把 linux-armv7-sdk2020 的构建产物整理成"可直接拷上板"的部署包
#
#  用法（在 Linux 构建机上，通常是 PetaLinux SDK 所在的 VM）：
#      scripts/deploy-armv7.sh [构建目录] [输出 tar.gz]
#
#  默认：
#      构建目录 build/linux-armv7-sdk2020
#      输出     dist/board_deploy-armv7.tar.gz
#
#  打包内容：
#      bin/     9 个可执行（strip 后）
#      lib/     非系统动态库（ORT / OpenCV，按 readelf NEEDED 自动收集）
#      models/  测试模型（含 ml golden）
#      run.sh   设好 LD_LIBRARY_PATH / MYLIB_TEST_MODELS_DIR 的启动器
#      verify.sh 板端一键回归
#
#  上板：
#      scp dist/board_deploy-armv7.tar.gz root@<板子IP>:/home/root/
#      ssh root@<板子IP> 'cd /home/root && tar xzf board_deploy-armv7.tar.gz'
#      ssh root@<板子IP> 'cd /home/root/board_deploy && ./verify.sh'
# =============================================================================
set -euo pipefail

BUILD_DIR="${1:-build/linux-armv7-sdk2020}"
OUT_TAR="${2:-dist/board_deploy-armv7.tar.gz}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

[ -d "$BUILD_DIR/bin" ] || { echo "找不到构建产物: $BUILD_DIR/bin" >&2; exit 1; }

# strip：优先用 SDK 里配套的交叉 binutils，找不到就退回 PATH 上的
STRIP=""
for c in "${CROSS_COMPILE:-}strip" arm-xilinx-linux-gnueabi-strip aarch64-none-linux-gnu-strip arm-linux-gnueabihf-strip arm-xilinx-linux-gnueabi-strip.exe aarch64-none-linux-gnu-strip.exe arm-linux-gnueabihf-strip.exe strip; do
    if command -v "$c" >/dev/null 2>&1; then STRIP="$c"; break; fi
done
[ -n "$STRIP" ] || { echo "找不到 strip（交叉 binutils）" >&2; exit 1; }

READELF="${STRIP%strip}readelf"
command -v "$READELF" >/dev/null 2>&1 || READELF=readelf

# strip 找不到交叉 binutils 时不致命：只警告，产物大一点但能跑
strip_file() {
    if ! "$STRIP" --strip-unneeded "$1" 2>/dev/null; then
        echo "  [warn] strip 失败（交叉 binutils 不在 PATH？）: $1" >&2
    fi
}

STAGE_ROOT="$(mktemp -d)"
trap 'rm -rf "$STAGE_ROOT"' EXIT
STAGE="$STAGE_ROOT/board_deploy"      # 打包后解压出来就是一个独立目录
mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/models"

# ---------------------------------------------------------------- 可执行文件
EXES="mylib_tests example_image example_signal example_ml example_net example_http example_pipeline example_opencv model_inspect"
for e in $EXES; do
    if [ -f "$BUILD_DIR/bin/$e" ]; then
        cp "$BUILD_DIR/bin/$e" "$STAGE/bin/$e"
        strip_file "$STAGE/bin/$e"
    else
        echo "  [skip] $e（未构建）"
    fi
done

# ---------------------------------------------------------------- 动态库
# 只收集非系统的（libc/libstdc++/libm/pthread/dl/rt 由板子提供）
is_system_lib() {
    case "$1" in
        libc.so*|libm.so*|libdl.so*|librt.so*|libpthread.so*|libstdc++.so*|libgcc_s.so*|ld-linux*) return 0 ;;
        *) return 1 ;;
    esac
}

COPY_SO=$(mktemp)
for exe in "$STAGE"/bin/*; do
    "$READELF" -d "$exe" 2>/dev/null | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p' || true
done | sort -u | while read -r so; do
    is_system_lib "$so" && continue
    echo "$so"
done > "$COPY_SO"

for so in $(cat "$COPY_SO"); do
    # 在 third_party 下按文件名找（ORT / OpenCV / ncnn）
    # 先按 SONAME 精确找；找不到再按去掉最后一段的前缀找
    # （NEEDED 是 libopencv_core.so.412，实际文件是 libopencv_core.so.4.12.0）
    src=$(find third_party -maxdepth 4 -name "$so" -type f 2>/dev/null | head -1)
    [ -n "$src" ] || src=$(find third_party -maxdepth 4 -name "$so" -type l 2>/dev/null | head -1)
    if [ -z "$src" ]; then
        prefix="${so%.*}"
        src=$(find third_party -maxdepth 4 -name "${prefix}*" -type f 2>/dev/null | head -1)
    fi
    if [ -n "$src" ]; then
        cp -L "$src" "$STAGE/lib/$so"
        echo "  [lib ] $so  <- $src"
    else
        echo "  [warn] $so 在 third_party 下没找到，请手工补齐" >&2
    fi
done
rm -f "$COPY_SO"
for so in "$STAGE"/lib/*.so*; do [ -e "$so" ] && strip_file "$so"; done

# ---------------------------------------------------------------- 模型
if [ -d models ]; then
    cp -r models/* "$STAGE/models/"
fi

# ---------------------------------------------------------------- 启动器
cat > "$STAGE/run.sh" <<'EOF'
#!/bin/sh
# 用法: ./run.sh mylib_tests --suite=dsp   |   ./run.sh example_ml models/ml/iris_logreg.onnx
DIR=$(cd "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH="$DIR/lib:$LD_LIBRARY_PATH"
export MYLIB_TEST_MODELS_DIR="$DIR/models"
exec "$DIR/bin/$@"
EOF

cat > "$STAGE/verify.sh" <<'EOF'
#!/bin/sh
# 板端一键回归：全部测试套件 + 主要示例
DIR=$(cd "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH="$DIR/lib:$LD_LIBRARY_PATH"
export MYLIB_TEST_MODELS_DIR="$DIR/models"

echo "===== 单元测试 ====="
"$DIR/bin/mylib_tests" || { echo "测试未全过"; exit 1; }

echo
echo "===== 示例 ====="
for ex in example_image example_signal example_net example_http; do
    [ -x "$DIR/bin/$ex" ] || continue
    echo "--- $ex ---"
    "$DIR/bin/$ex" >/dev/null 2>&1 && echo "  ok" || echo "  FAILED"
done
[ -x "$DIR/bin/example_ml" ] && {
    echo "--- example_ml ---"
    "$DIR/bin/example_ml" "$DIR/models/ml/iris_logreg.onnx" >/dev/null 2>&1 && echo "  ok" || echo "  FAILED"
}

echo
echo "===== 延迟参考 ====="
[ -x "$DIR/bin/model_inspect" ] && {
    for m in "$DIR/models/ml/iris_logreg.onnx" "$DIR/models/ml/gbdt_20f.onnx"; do
        [ -f "$m" ] || continue
        "$DIR/bin/model_inspect" "$m" --run --bench 100 2>/dev/null | tail -1 | sed "s#^#  $m: #"
    done
}
exit 0
EOF
chmod +x "$STAGE/run.sh" "$STAGE/verify.sh"

# ---------------------------------------------------------------- 打包
mkdir -p "$(dirname "$OUT_TAR")"
abs_out="$(cd "$(dirname "$OUT_TAR")" && pwd)/$(basename "$OUT_TAR")"
tar -C "$STAGE_ROOT" -czf "$abs_out" board_deploy

echo
echo "部署包: $abs_out  ($(du -h "$abs_out" | cut -f1))"
echo "  可执行: $(ls "$STAGE/bin" | tr '\n' ' ')"
echo "  动态库: $(ls "$STAGE/lib" | tr '\n' ' ')"
