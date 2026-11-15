#!/usr/bin/env bash
# =============================================================================
#  下载 resources/ 里缺失的三方库压缩包
#
#  为什么需要它：resources/*.zip 不进 git（体积大），所以 clone 之后必须补一次，
#  CI 也一样。离线环境下跳过本脚本，手工把 zip 放进 resources/ 即可。
#
#  用法：
#      scripts/fetch-deps.sh                 # 只补必需的三个（eigen / spdlog / json）
#      scripts/fetch-deps.sh --with-ort-win  # 额外下载 Windows x64 的 ONNX Runtime
#      scripts/fetch-deps.sh --all           # 以上全部
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/resources"
mkdir -p "$DEST"

WITH_ORT_WIN=0
for arg in "$@"; do
    case "$arg" in
        --with-ort-win|--all) WITH_ORT_WIN=1 ;;
        -h|--help) sed -n '2,12p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "未知参数: $arg" >&2; exit 2 ;;
    esac
done

fetch() {
    local name="$1" url="$2"
    if [ -s "$DEST/$name" ]; then
        echo "[skip] $name（已存在）"
        return 0
    fi
    echo "[get ] $name"
    # 先写 .part 再 rename：断流时不会留下一个"看起来完整"的坏压缩包
    curl -fL --retry 3 --retry-delay 2 -o "$DEST/$name.part" "$url"
    mv "$DEST/$name.part" "$DEST/$name"
}

fetch eigen-5.0.0.zip   "https://gitlab.com/libeigen/eigen/-/archive/5.0.0/eigen-5.0.0.zip"
fetch spdlog-1.17.0.zip "https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.zip"
fetch json-include.zip  "https://github.com/nlohmann/json/releases/download/v3.12.0/include.zip"

if [ "$WITH_ORT_WIN" = "1" ]; then
    fetch onnxruntime-win-x64-1.23.2.zip \
          "https://github.com/microsoft/onnxruntime/releases/download/v1.23.2/onnxruntime-win-x64-1.23.2.zip"
fi

echo
echo "完成。解压目录: $ROOT/third_party（首次 cmake configure 时自动生成）"
