#!/usr/bin/env bash
# =============================================================================
#  板端部署 + 验证（密码认证版，走 WSL + sshpass）
#
#  为什么用 WSL：Windows 没有 sshpass，密码认证在非交互环境里过不去。
#  如果你已经配好 ssh 密钥，也可以用 scripts/deploy-and-bench.ps1。
#
#  用法：
#    wsl -d Ubuntu-24.04 -u root -- bash scripts/board-deploy-wsl.sh
#    BOARD=192.168.0.130 BOARD_PASS=root bash scripts/board-deploy-wsl.sh
#
#  前置：
#    - 静态产物：build/linux-armv7-static/bin/{mylib_tests,model_inspect}
#      （板子 glibc 2.30 < PC 上 SDK 的 2.39，动态链接跑不起来，必须静态）
#    - 模型：models/ncnn/*.param|.bin
# =============================================================================
set -uo pipefail

BOARD="${BOARD:-192.168.0.130}"
PASS="${BOARD_PASS:-root}"
LOCAL="${LOCAL_DIR:-/mnt/d/syncworkspace/cpp_my_cpp_libs}"
BIN="$LOCAL/build/linux-armv7-static/bin"
DEST="${DEST:-/tmp/mylib}"

SSH="sshpass -p $PASS ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 -o LogLevel=ERROR"
SCP="sshpass -p $PASS scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"

die() { echo "!! $*" >&2; exit 1; }

[ -x "$BIN/mylib_tests" ] || die "找不到静态产物，先构建：
  cmake -S . -B build/linux-armv7-static -G Ninja \\
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-armv7-zynq.cmake \\
    -DCMAKE_BUILD_TYPE=Release -DMYLIB_ENABLE_ONNXRUNTIME=OFF -DMYLIB_ENABLE_NCNN=ON \\
    -DMYLIB_BUILD_EXAMPLES=OFF -DCMAKE_EXE_LINKER_FLAGS=-static
  cmake --build build/linux-armv7-static"

command -v sshpass >/dev/null || die "缺少 sshpass：apt-get install sshpass"

echo "==> 连接 $BOARD"
$SSH "root@$BOARD" "echo ok" >/dev/null || die "ssh 连不上"

echo "==> 准备目录 $DEST"
$SSH "root@$BOARD" "rm -rf $DEST && mkdir -p $DEST/models/ncnn"

echo "==> 推送产物"
$SCP "$BIN/mylib_tests" "$BIN/model_inspect" "root@$BOARD:$DEST/" >/dev/null
for m in small_cnn bench_cnn innerproduct; do
    if [ -f "$LOCAL/models/ncnn/$m.param" ]; then
        $SCP "$LOCAL/models/ncnn/$m.param" "$LOCAL/models/ncnn/$m.bin" \
             "root@$BOARD:$DEST/models/ncnn/" >/dev/null
    fi
done
$SSH "root@$BOARD" "chmod +x $DEST/mylib_tests $DEST/model_inspect"

echo
echo "========== 环境 =========="
$SSH "root@$BOARD" "uname -m; nproc; free -m | sed -n 2p"

echo
echo "========== 单元测试 =========="
$SSH "root@$BOARD" "cd $DEST && ./mylib_tests 2>&1 | tail -2"

echo
echo "========== small_cnn（32x32，固定开销参考）=========="
$SSH "root@$BOARD" "cd $DEST && ./model_inspect models/ncnn/small_cnn.param --run --shape 1,3,32,32 --bench 200 2>&1 | tail -2"

echo
echo "========== bench_cnn（224x224，≈MobileNetV2 规模）=========="
$SSH "root@$BOARD" "cd $DEST && ./model_inspect models/ncnn/bench_cnn.param --run --shape 1,3,224,224 --bench 5 2>&1 | tail -2"

echo
echo "========== 内存峰值 =========="
$SSH "root@$BOARD" "cd $DEST && /usr/bin/time -v ./model_inspect models/ncnn/bench_cnn.param --run --shape 1,3,224,224 2>&1 | grep -i 'maximum resident' || echo n/a"

echo
echo "完成。"
