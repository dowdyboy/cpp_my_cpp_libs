#!/usr/bin/env python3
# =============================================================================
#  比对两个后端的输出（model_inspect --dump 生成的文本）
#
#  用法:
#      python tools/compare_outputs.py a.txt b.txt
#
#  输出: 逐输出的最大绝对误差 / 平均绝对误差 / 相对误差，以及是否在容差内。
# =============================================================================

import sys


def read_dump(path):
    """返回 [(name, shape, [values...]), ...]"""
    outs = []
    cur = None
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith("#"):
                if cur:
                    outs.append(cur)
                parts = line[1:].split()
                name = parts[0] if parts else "?"
                shape = " ".join(parts[1:]) if len(parts) > 1 else "?"
                cur = (name, shape, [])
            elif cur is not None:
                cur[2].append(float(line))
    if cur:
        outs.append(cur)
    return outs


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2

    a = read_dump(sys.argv[1])
    b = read_dump(sys.argv[2])

    if len(a) != len(b):
        print(f"输出个数不同: {len(a)} vs {len(b)}")
        return 1

    ok = True
    for (na, sa, va), (nb, sb, vb) in zip(a, b):
        print(f"\n输出 {na}  shape={sa} vs {nb}  shape={sb}")
        if len(va) != len(vb):
            print(f"  元素个数不同: {len(va)} vs {len(vb)}")
            ok = False
            continue

        max_abs = 0.0
        sum_abs = 0.0
        max_rel = 0.0
        for x, y in zip(va, vb):
            d = abs(x - y)
            max_abs = max(max_abs, d)
            sum_abs += d
            denom = max(abs(x), abs(y), 1e-6)
            max_rel = max(max_rel, d / denom)

        mean_abs = sum_abs / len(va)
        # 经验阈值：fp32 上跨实现通常 < 1e-4；放大到 1e-3 容忍不同优化顺序
        verdict = "一致" if max_abs < 1e-3 else ("接近" if max_abs < 1e-2 else "不一致")
        if max_abs >= 1e-2:
            ok = False

        print(f"  max_abs={max_abs:.3e}  mean_abs={mean_abs:.3e}  max_rel={max_rel:.3e}  -> {verdict}")
        print("  前 4 个:  A=" + " ".join(f"{v:.6f}" for v in va[:4]))
        print("            B=" + " ".join(f"{v:.6f}" for v in vb[:4]))

    print("\n结论:", "通过" if ok else "不通过")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
