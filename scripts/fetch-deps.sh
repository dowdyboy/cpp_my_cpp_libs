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

# stb 没有发布 tag，上游只提供 master 上的两个 .h，所以这里按 commit 固定，
# 下载后校验 SHA256，再打成本工程的 zip（cmake 只认 resources/*.zip）。
# 换版本 = 同时改 PIN / SHA / zip 名字三处，缺一不可。
STB_PIN="9600522de53973336fa033a289858a4354cbb04c"
STB_ZIP="stb-image-2.32.zip"
fetch_stb() {
    if [ -s "$DEST/$STB_ZIP" ]; then
        echo "[skip] $STB_ZIP（已存在）"
        return 0
    fi
    echo "[get ] $STB_ZIP（stb @ $STB_PIN，两个头文件）"
    local tmp
    tmp="$(mktemp -d)"
    curl -fL --retry 3 --retry-delay 2 -o "$tmp/stb_image.h" \
        "https://raw.githubusercontent.com/nothings/stb/$STB_PIN/stb_image.h"
    curl -fL --retry 3 --retry-delay 2 -o "$tmp/stb_image_write.h" \
        "https://raw.githubusercontent.com/nothings/stb/$STB_PIN/stb_image_write.h"

    echo "DEB4F18C9CA21B751A1323019031C94C0BF5CA1D2929EF5913CC56F00AAB6E91  $tmp/stb_image.h" \
        | sha256sum -c -
    echo "A8E7F77F0CCFDC9BAC09A71268B131931FF81AAFCA82C393AEB6AABA10D9E7CA  $tmp/stb_image_write.h" \
        | sha256sum -c -

    if command -v python3 >/dev/null 2>&1; then
        python3 - "$tmp" "$DEST/$STB_ZIP" <<'PY'
import pathlib, sys, zipfile
tmp, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for name in ("stb_image.h", "stb_image_write.h"):
        z.write(tmp / name, name)
PY
    elif command -v zip >/dev/null 2>&1; then
        (cd "$tmp" && zip -q -X "$DEST/$STB_ZIP" stb_image.h stb_image_write.h)
    else
        echo "需要 python3 或 zip 才能打包 $STB_ZIP" >&2
        rm -rf "$tmp"
        return 1
    fi
    rm -rf "$tmp"
}
fetch_stb

# cpp-httplib（HTTP）：单头文件 + LICENSE，commit pin + SHA256
HTTPLIB_PIN="cdcd2ac6e6d5548b5a9130fb39ab76d2c6c614b0"
HTTPLIB_ZIP="httplib-0.66.0.zip"
fetch_httplib() {
    if [ -s "$DEST/$HTTPLIB_ZIP" ]; then
        echo "[skip] $HTTPLIB_ZIP（已存在）"
        return 0
    fi
    echo "[get ] $HTTPLIB_ZIP（cpp-httplib @ $HTTPLIB_PIN）"
    local tmp
    tmp="$(mktemp -d)"
    curl -fL --retry 3 --retry-delay 2 -o "$tmp/httplib.h" \
        "https://raw.githubusercontent.com/yhirose/cpp-httplib/$HTTPLIB_PIN/httplib.h"
    curl -fL --retry 3 --retry-delay 2 -o "$tmp/LICENSE" \
        "https://raw.githubusercontent.com/yhirose/cpp-httplib/$HTTPLIB_PIN/LICENSE"
    echo "51DDD1482C67ACB31F3064E48329AADE14ACC7DD0FAB31EE5397193697EA1C35  $tmp/httplib.h" | sha256sum -c -
    echo "4B45CBE16D7B71B89AE6127E26E0D90A029198CA5E958AD8E3D0B8BBED364D8B  $tmp/LICENSE" | sha256sum -c -
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$tmp" "$DEST/$HTTPLIB_ZIP" <<'PY'
import pathlib, sys, zipfile
tmp, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for name in ("httplib.h", "LICENSE"):
        z.write(tmp / name, name)
PY
    else
        echo "需要 python3 才能打包 $HTTPLIB_ZIP" >&2
        rm -rf "$tmp"
        return 1
    fi
    rm -rf "$tmp"
}
fetch_httplib

# pocketfft（信号处理）：同上，单头文件 + commit pin + SHA256
PFF_PIN="4d865cba24359b05b37da4e75aee24498c3b78e7"
PFF_ZIP="pocketfft-4d865cb.zip"
fetch_pocketfft() {
    if [ -s "$DEST/$PFF_ZIP" ]; then
        echo "[skip] $PFF_ZIP（已存在）"
        return 0
    fi
    echo "[get ] $PFF_ZIP（pocketfft @ $PFF_PIN）"
    local tmp
    tmp="$(mktemp -d)"
    curl -fL --retry 3 --retry-delay 2 -o "$tmp/pocketfft_hdronly.h" \
        "https://raw.githubusercontent.com/mreineck/pocketfft/$PFF_PIN/pocketfft_hdronly.h"
    echo "518A95C552AEAEE352AA30122137CD9E135D23C91E2863D9272AAA758744C86D  $tmp/pocketfft_hdronly.h" \
        | sha256sum -c -

    if command -v python3 >/dev/null 2>&1; then
        python3 - "$tmp" "$DEST/$PFF_ZIP" <<'PY'
import pathlib, sys, zipfile
tmp, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    z.write(tmp / "pocketfft_hdronly.h", "pocketfft_hdronly.h")
PY
    else
        echo "需要 python3 才能打包 $PFF_ZIP" >&2
        rm -rf "$tmp"
        return 1
    fi
    rm -rf "$tmp"
}
fetch_pocketfft

if [ "$WITH_ORT_WIN" = "1" ]; then
    fetch onnxruntime-win-x64-1.23.2.zip \
          "https://github.com/microsoft/onnxruntime/releases/download/v1.23.2/onnxruntime-win-x64-1.23.2.zip"
fi

echo
echo "完成。解压目录: $ROOT/third_party（首次 cmake configure 时自动生成）"
