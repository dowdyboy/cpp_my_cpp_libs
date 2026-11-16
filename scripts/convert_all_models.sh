#!/usr/bin/env bash
# 把 models/*.onnx 批量转成 ncnn 的 .param/.bin，并记录每个模型能否转换
# 用法: bash scripts/convert_all_models.sh [models 目录]
set -uo pipefail

MODELS="${1:-$(cd "$(dirname "$0")/.." && pwd)/models}"
OUT="$MODELS/ncnn"
TOOL=/opt/ncnn-tools/ncnn-build/tools/onnx/onnx2ncnn

mkdir -p "$OUT"
printf "%-16s %-8s %s\n" "模型" "结果" "备注"
printf -- "------------------------------------------------------------\n"

for f in "$MODELS"/*.onnx; do
    name=$(basename "$f" .onnx)
    log=$(mktemp)
    if "$TOOL" "$f" "$OUT/$name.param" "$OUT/$name.bin" >"$log" 2>&1; then
        # onnx2ncnn 有时 rc=0 但输出里带 Unsupported
        if grep -qi "unsupported\|not supported\|fail" "$log"; then
            # 有警告说明有层被丢掉了，转出来的 param 加载时会崩 —— 直接删掉，
            # 免得后面调试时以为是运行时的问题
            printf "%-16s %-8s %s\n" "$name" "有警告" "$(grep -i 'unsupported\|not supported\|fail' "$log" | head -1 | cut -c1-70)"
            rm -f "$OUT/$name.param" "$OUT/$name.bin"
        else
            printf "%-16s %-8s param=%s bin=%s\n" "$name" "OK" \
                "$(stat -c%s "$OUT/$name.param")B" "$(stat -c%s "$OUT/$name.bin")B"
        fi
    else
        printf "%-16s %-8s %s\n" "$name" "失败" "$(tail -2 "$log" | head -1 | cut -c1-70)"
        rm -f "$OUT/$name.param" "$OUT/$name.bin"
    fi
    rm -f "$log"
done
