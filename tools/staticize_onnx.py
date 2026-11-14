#!/usr/bin/env python3
# =============================================================================
#  把 ONNX 模型的动态 batch 维固定成 1
#
#  为什么需要：ncnn 没有 batch 维，onnx2ncnn 遇到动态维会退化出
#  Shape/Unsqueeze 这类它不支持的层，转换结果跑不起来。
#  把 dim_param 换成具体数字后，不少模型就能干净地转成 ncnn。
#
#  用法:
#      python tools/staticize_onnx.py in.onnx out.onnx [batch]
# =============================================================================

import sys

import onnx


def fix_value_info(vi, batch: int) -> bool:
    changed = False
    tt = vi.type.tensor_type
    if not tt.HasField("shape"):
        return False
    dims = tt.shape.dim
    if len(dims) == 0:
        return False
    d0 = dims[0]
    # 动态维：dim_param 非空，或者 dim_value <= 0
    if d0.dim_param or d0.dim_value <= 0:
        d0.Clear()
        d0.dim_value = batch
        changed = True
    for d in list(dims)[1:]:
        if d.dim_param:
            # 其余动态维无法猜测，直接报出来
            print(f"  [警告] {vi.name} 还有动态维 '{d.dim_param}'，未处理")
    return changed


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2

    src, dst = sys.argv[1], sys.argv[2]
    batch = int(sys.argv[3]) if len(sys.argv) > 3 else 1

    model = onnx.load(src)
    n = 0
    for vi in list(model.graph.input) + list(model.graph.output):
        if fix_value_info(vi, batch):
            n += 1
            print(f"  固定 {vi.name} -> batch={batch}")

    if n == 0:
        print("  没有发现需要固定的动态 batch 维")

    onnx.save(model, dst)
    print(f"已保存 {dst}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
