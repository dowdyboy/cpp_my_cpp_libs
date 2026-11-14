#!/usr/bin/env python3
# =============================================================================
#  生成一个 ncnn 测试模型（.param + .bin），不需要 protobuf / onnx2ncnn
#
#  模型：y = InnerProduct(x, W, b)，输入 [8] -> 输出 [10]
#  .bin 是裸 float32：先 W（10x8 行优先），再 b（10）
#
#  同时打印 x = 0.5 时的期望输出，供 C++ 侧比对。
#
#  用法：
#      python tools/gen_ncnn_test_model.py [输出目录]
# =============================================================================

import os
import struct
import sys

import numpy as np

OUT_DIR = sys.argv[1] if len(sys.argv) > 1 else "models/ncnn"

MAGIC = 7767517


def main() -> int:
    os.makedirs(OUT_DIR, exist_ok=True)

    rng = np.random.default_rng(7)
    num_output = 10
    input_size = 8

    W = rng.normal(0, 0.5, size=(num_output, input_size)).astype(np.float32)
    b = rng.normal(0, 0.1, size=(num_output,)).astype(np.float32)

    param_path = os.path.join(OUT_DIR, "innerproduct.param")
    bin_path = os.path.join(OUT_DIR, "innerproduct.bin")

    # .param 是文本格式：
    #   7767517
    #   <layer_count> <blob_count>
    #   <type> <name> <bottom_count> <top_count> <bottoms...> <tops...> [k=v ...]
    param = (
        f"{MAGIC}\n"
        "2 2\n"
        "Input        data    0 1 data\n"
        f"InnerProduct fc      1 1 data output 0={num_output} 1=1 2={num_output * input_size}\n"
    )
    with open(param_path, "w", encoding="ascii") as f:
        f.write(param)

    # .bin 格式要点（踩过坑）：
    #   ncnn 的 ModelBin 读权重时会先读 4 字节 tag：
    #     0x01306B47 -> fp16，0x000D4B38 -> int8，tag 首字节为 0 -> 裸 float32
    #   所以裸 float32 权重要在前面补 4 个零字节，否则会被当成未知格式而加载失败。
    #   偏置走的是 type=1（不读 tag），直接写裸 float32。
    with open(bin_path, "wb") as f:
        f.write(struct.pack("<I", 0))
        f.write(W.tobytes())
        f.write(b.tobytes())

    # 期望值：x = 0.5，y = W @ x + b
    x = np.full(input_size, 0.5, dtype=np.float32)
    y = W @ x + b

    print(f"已生成 {param_path}")
    print(f"已生成 {bin_path} ({os.path.getsize(bin_path)} 字节)")
    print("输入 x = " + " ".join(f"{v:.4f}" for v in x))
    print("期望输出 y = " + " ".join(f"{v:.6f}" for v in y))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
