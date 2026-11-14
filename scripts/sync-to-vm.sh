#!/usr/bin/env bash
# =============================================================================
#  把本机项目同步到 VMware 虚拟机（只带源码，不带构建产物/大模型）
#
#  目标布局：
#    ~/Projects/cpp_my_cpp_libs/
#      ├── src/     <- 本项目
#      ├── deps/    第三方源码
#      └── dist/   编好的产物
#
#  用法（在 WSL 里跑）：
#    cp scripts/vm-env.sh.example scripts/vm-env.sh   # 首次：填上密码
#    bash scripts/sync-to-vm.sh
# =============================================================================
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
[ -f "$HERE/vm-env.sh" ] && source "$HERE/vm-env.sh"

VM_HOST="${VM_HOST:?请先创建 scripts/vm-env.sh（参考 .example）}"
VM_USER="${VM_USER:-lfbhlx}"
VM_PASS="${VM_PASS:?缺少 VM_PASS}"
VM_PROJ="${VM_PROJ:-/home/lfbhlx/Projects/cpp_my_cpp_libs}"

SRC="${SRC:-$(cd "$HERE/.." && pwd)}"
DST="$VM_PROJ/src"

SSH="sshpass -p $VM_PASS ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=15 -o LogLevel=ERROR $VM_USER@$VM_HOST"

echo "==> 同步 $SRC -> $VM_HOST:$DST"

cd "$SRC" || exit 1
# 注意：--exclude 必须写在文件列表前面，GNU tar 的选项是位置相关的
tar czf - \
    --exclude='models/real' --exclude='scripts/vm-env.sh' \
    CMakeLists.txt CMakePresets.json README.md .gitignore \
    cmake include src examples tests tools scripts docs models \
    third_party/eigen-5.0.0 third_party/spdlog-1.17.0 \
    third_party/nlohmann-json-3.12.0 third_party/ncnn-src \
  | $SSH "rm -rf '$DST' && mkdir -p '$DST' && tar xzf - -C '$DST' && echo '  done'"

echo "==> 校验"
$SSH "du -sh '$DST'; ls '$DST' | head -6"
