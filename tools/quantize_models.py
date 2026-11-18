#!/usr/bin/env python3
"""把 ONNX 模型量化成 Int8，并给出体积/精度对照。

为什么需要这个脚本
------------------
docs/04 §8 把 Int8 量化列为"性价比第一名"（2~3x 速度、1/4 体积），但在此之前
**一个数字都没有**。这里把整条链路固化下来，避免"每次都要重新查 ORT 量化 API"。

做三件事
--------
1. `quantize_static`（QDQ 格式，per-channel）—— 需要校准集；
2. `quantize_dynamic`（无校准集，只量化权重）—— 作为对照基线；
3. 用同一个 ORT 会话跑 fp32 / int8 各 N 次前向，报告：
   体积、最大绝对误差、平均绝对误差、余弦相似度、分类 top-1 一致率。

校准数据从哪来
--------------
脚本默认用**合成数据**（均匀噪声 / 梯度 / 棋盘 / 常量），这只够验证"流程通"，
数值指标不能当成真实精度。要拿到可信数字，用 `--calib-dir` 指向真实图片目录
（会被 letterbox 到模型输入尺寸），或用 `--calib-npy` 指向预先准备好的 ndarray。

用法
----
    conda run -n t2xpu python tools/quantize_models.py                 # 默认模型集
    conda run -n t2xpu python tools/quantize_models.py --models models/bench_cnn.onnx
    conda run -n t2xpu python tools/quantize_models.py --calib-dir data/calib --calib 200
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time

import numpy as np
import onnx
import onnxruntime as ort
from onnxruntime.quantization import (
    CalibrationDataReader,
    QuantFormat,
    QuantType,
    quantize_dynamic,
    quantize_static,
)


# --------------------------------------------------------------------------- 模型表
# 只列"量化有意义"的模型：CNN 在 ORT 上走 QLinearConv/QDQ 图，收益明显；
# 表格模型（ai.onnx.ml 的 TreeEnsemble）不参与量化，量化器会原样放过。
DEFAULT_MODELS = [
    {"path": "models/small_cnn.onnx", "input": "input", "kind": "cnn"},
    {"path": "models/bench_cnn.onnx", "input": "input", "kind": "cnn"},
    {"path": "models/real/mobilenetv2-12.onnx", "input": "input", "kind": "cnn", "optional": True},
]


# --------------------------------------------------------------------------- 校准集
class SyntheticReader(CalibrationDataReader):
    """合成校准数据。

    每种模型给几种"有结构"的输入，而不是纯白噪声：纯噪声的激活分布和真实图像差得远，
    量化参数（scale/zero_point）会偏，测出来的误差没有参考价值。
    """

    def __init__(self, input_name: str, shape: list[int], count: int, seed: int = 1234):
        self.input_name = input_name
        self.shape = shape
        self.count = count
        self.index = 0
        rng = np.random.default_rng(seed)

        samples = []
        base = np.zeros(shape, dtype=np.float32)
        for i in range(count):
            mode = i % 4
            if mode == 0:                      # 均匀噪声
                x = rng.random(shape, dtype=np.float32)
            elif mode == 1:                    # 水平梯度
                g = np.linspace(0.0, 1.0, shape[-1], dtype=np.float32)
                x = np.broadcast_to(g, shape).copy()
            elif mode == 2:                    # 棋盘
                x = base.copy()
                x[..., ::2, ::2] = 1.0
                x[..., 1::2, 1::2] = 1.0
            else:                              # 低频正弦
                yy = np.linspace(0, 6.28, shape[-2], dtype=np.float32)[:, None]
                xx = np.linspace(0, 6.28, shape[-1], dtype=np.float32)[None, :]
                x = np.broadcast_to(np.sin(yy + xx), shape).astype(np.float32) * 0.5 + 0.5
            samples.append(x)
        self.samples = samples

    def get_next(self):
        if self.index >= len(self.samples):
            return None
        x = self.samples[self.index]
        self.index += 1
        return {self.input_name: x}

    def rewind(self):
        self.index = 0


def ensure_opset13(src: str, dst: str) -> str:
    """把模型升到 opset >= 13。

    为什么必须做：`QuantizeLinear` / `DequantizeLinear` 的 **`axis` 属性是 opset 13 才引入的**。
    ORT 的量化器不看你模型的 opset，照样会写出带 `axis` 的 Q/DQ 节点；于是 opset 12 的模型
    （比如 ORT Model Zoo 的 mobilenetv2-12）量化出来是**非法图**，加载时报：

        INVALID_GRAPH ... Error Unrecognized attribute: axis for operator DequantizeLinear

    这个错误在量化阶段不会出现，只在"加载量化后的模型"时才炸 —— 很容易踩。
    """
    import onnx

    m = onnx.load(src)
    default_opsets = [o.version for o in m.opset_import if o.domain in ("", "ai.onnx")]
    current = max(default_opsets) if default_opsets else 0
    if current >= 13:
        return src

    from onnx import version_converter

    upgraded = version_converter.convert_version(m, 13)
    onnx.save(upgraded, dst)
    print(f"  opset {current} -> 13（Q/DQ 的 axis 属性需要 opset>=13），已存 {os.path.basename(dst)}")
    return dst


def make_reader(model_path: str, input_name: str, count: int, calib_dir: str | None,
                calib_npy: str | None) -> CalibrationDataReader:
    if calib_npy:
        arr = np.load(calib_npy)
        if arr.dtype != np.float32:
            arr = arr.astype(np.float32)

        class ArrayReader(CalibrationDataReader):
            def __init__(self):
                self.i = 0

            def get_next(self):
                if self.i >= len(arr):
                    return None
                v = arr[self.i]
                self.i += 1
                return {input_name: v[None, ...]}

            def rewind(self):
                self.i = 0

        return ArrayReader()

    if calib_dir:
        # 真实图片校准：只做最基本的读图 + resize，避免引入 cv2 依赖
        from PIL import Image  # noqa: PL0410  (PIL 在 t2xpu 里通常有；没有就报错提示)

        shape = concrete_shape(model_path, input_name)
        w, h = shape[-1], shape[-2]
        files = sorted(
            os.path.join(calib_dir, f) for f in os.listdir(calib_dir)
            if f.lower().endswith((".png", ".jpg", ".jpeg", ".bmp"))
        )[:count]
        if not files:
            raise SystemExit(f"{calib_dir} 里没有图片")
        samples = []
        for f in files:
            img = Image.open(f).convert("RGB").resize((w, h))
            x = np.asarray(img, dtype=np.float32) / 255.0
            x = x.transpose(2, 0, 1)[None, ...]  # HWC -> NCHW
            samples.append(x)

        class DirReader(CalibrationDataReader):
            def __init__(self):
                self.i = 0

            def get_next(self):
                if self.i >= len(samples):
                    return None
                v = samples[self.i]
                self.i += 1
                return {input_name: v}

            def rewind(self):
                self.i = 0

        return DirReader()

    m = onnx.load(model_path, load_external_data=False)
    shape = None
    for i in m.graph.input:
        if i.name == input_name:
            shape = [d.dim_value if d.dim_value > 0 else 1 for d in i.type.tensor_type.shape.dim]
    if not shape:
        raise SystemExit(f"{model_path}: 找不到输入 {input_name} 的静态形状，请用 --calib-npy")
    return SyntheticReader(input_name, shape, count)


def concrete_shape(model_path: str, input_name: str) -> list[int]:
    m = onnx.load(model_path, load_external_data=False)
    for i in m.graph.input:
        if i.name == input_name:
            return [d.dim_value if d.dim_value > 0 else 1 for d in i.type.tensor_type.shape.dim]
    raise SystemExit(f"{model_path}: 找不到输入 {input_name}")


# --------------------------------------------------------------------------- 指标
def cosine(a: np.ndarray, b: np.ndarray) -> float:
    a = a.ravel().astype(np.float64)
    b = b.ravel().astype(np.float64)
    na, nb = np.linalg.norm(a), np.linalg.norm(b)
    if na == 0 or nb == 0:
        return 1.0 if na == nb else 0.0
    return float(np.dot(a, b) / (na * nb))


def not_quantizable_reason(path: str) -> str | None:
    """判断模型是否适用 Int8 量化；适用返回 None，否则返回原因。

    典型的"不适用"：`ai.onnx.ml` 的表格模型（TreeEnsemble/SVM/线性模型混合）。
    它们的 opset_import 里**只有 `ai.onnx.ml` 域**，ORT 的量化器会直接抛
    `ValueError: Failed to find proper ai.onnx domain`。与其让它崩掉整批，
    不如显式跳过并说清原因。
    """
    m = onnx.load(path, load_external_data=False)
    domains = {o.domain for o in m.opset_import}
    if "" not in domains and "ai.onnx" not in domains:
        return f"没有 ai.onnx 域（只有 {sorted(domains or {'?'})}），表格/传统 ML 模型不适用量化"
    has_conv = any(n.op_type in ("Conv", "ConvTranspose", "MatMul", "Gemm") for n in m.graph.node)
    if not has_conv:
        return "图里没有 Conv/MatMul/Gemm（量化收益≈0，且容易产出 ORT 未实现的算子）"
    return None


def eval_samples_for(shape: list[int], count: int, seed: int = 11) -> list[np.ndarray]:
    rng = np.random.default_rng(seed)
    return [rng.random(shape, dtype=np.float32) for _ in range(count)]


def evaluate(fp32_path: str, int8_path: str, input_name: str, samples: list[np.ndarray]):
    so = ort.SessionOptions()
    so.log_severity_level = 3
    s32 = ort.InferenceSession(fp32_path, so, providers=["CPUExecutionProvider"])
    s8 = ort.InferenceSession(int8_path, so, providers=["CPUExecutionProvider"])
    max_abs, mean_abs, cosines, top1_agree = 0.0, 0.0, [], 0
    for x in samples:
        y32 = s32.run(None, {input_name: x})
        y8 = s8.run(None, {input_name: x})
        for a, b in zip(y32, y8):
            if a.dtype.kind not in "fc":
                continue  # 整型/标签输出不做浮点比较
            a = a.astype(np.float32)
            b = b.astype(np.float32)
            d = np.abs(a - b)
            max_abs = max(max_abs, float(d.max()))
            mean_abs += float(d.mean())
            cosines.append(cosine(a, b))
        # 分类任务：把第一个浮点输出当 logits/probabilities
        f32_out = next((y for y in y32 if y.dtype.kind == "f"), None)
        f8_out = next((y for y in y8 if y.dtype.kind == "f"), None)
        if f32_out is not None and f8_out is not None and f32_out.ndim == 2:
            if int(np.argmax(f32_out, axis=1)[0]) == int(np.argmax(f8_out, axis=1)[0]):
                top1_agree += 1

    n = max(1, len(samples))
    return {
        "max_abs_err": max_abs,
        "mean_abs_err": mean_abs / n,
        "cosine_min": min(cosines) if cosines else 1.0,
        "top1_agree": top1_agree / n,
    }


def latency(path: str, input_name: str, shape: list[int], iters: int = 30):
    so = ort.SessionOptions()
    so.log_severity_level = 3
    s = ort.InferenceSession(path, so, providers=["CPUExecutionProvider"])
    x = np.random.default_rng(7).random(shape, dtype=np.float32)
    for _ in range(3):  # 预热
        s.run(None, {input_name: x})
    t0 = time.perf_counter()
    for _ in range(iters):
        s.run(None, {input_name: x})
    return (time.perf_counter() - t0) / iters * 1000.0  # ms


# --------------------------------------------------------------------------- 主流程
def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", nargs="*", default=None, help="要量化的 .onnx（默认内置三个 CNN）")
    ap.add_argument("--calib", type=int, default=32, help="校准样本数（默认 32）")
    ap.add_argument("--calib-dir", default=None, help="用真实图片做校准（PNG/JPG）")
    ap.add_argument("--calib-npy", default=None, help="用 .npy 做校准（NCHW float32）")
    ap.add_argument("--out-dir", default="models/int8")
    ap.add_argument("--eval", type=int, default=8, help="精度对照用的样本数")
    # 动态量化默认关闭：对 CNN 它会产出 ConvInteger，而 ORT CPU EP **没有实现**
    # （实测报 NOT_IMPLEMENTED: Could not find an implementation for ConvInteger(10)）。
    # 官方推荐动态量化只用于 RNN/Transformer 这类 MatMul 为主的模型。
    ap.add_argument("--with-dynamic", action="store_true",
                    help="额外生成动态量化版本（仅对非 CNN 有意义）")
    args = ap.parse_args()

    models = DEFAULT_MODELS if not args.models else [
        {"path": p, "input": "input", "kind": "cnn"} for p in args.models
    ]
    os.makedirs(args.out_dir, exist_ok=True)

    rows = []
    for spec in models:
        path = spec["path"]
        if not os.path.exists(path):
            if spec.get("optional"):
                print(f"[skip] {path}（不存在）")
                continue
            print(f"[skip] {path}（不存在）")
            continue

        name = os.path.splitext(os.path.basename(path))[0]
        in_name = spec["input"]

        reason = not_quantizable_reason(path)
        if reason is not None:
            print(f"\n=== {name} ===\n  [skip] {reason}")
            continue

        shape = concrete_shape(path, in_name)
        print(f"\n=== {name}  shape={shape} ===")

        reader = make_reader(path, in_name, args.calib, args.calib_dir, args.calib_npy)
        qdq_path = os.path.join(args.out_dir, f"{name}_qdq.onnx")

        # 中间产物单独放 _work/，别把 models/int8 目录搞成垃圾场
        work = os.path.join(args.out_dir, "_work")
        os.makedirs(work, exist_ok=True)

        # 1) opset 升级（<13 的模型量化出来的 Q/DQ 是非法图，见 ensure_opset13 的注释）
        path13 = ensure_opset13(path, os.path.join(work, f"{name}_op13.onnx"))

        # 2) ORT 官方建议先做一遍预处理（常量折叠 / 形状推断），否则某些模型量化会失败
        pre_path = path13
        try:
            from onnxruntime.quantization.shape_inference import quant_pre_process

            pre_path = os.path.join(work, f"{name}_pre.onnx")
            quant_pre_process(path13, pre_path, skip_symbolic_shape=True)
        except Exception as e:  # noqa: BLE001 - 预处理失败就退回原模型，不阻断
            print(f"  [warn] 预处理失败（继续用未预处理的模型）: {e}")
            pre_path = path13

        try:
            quantize_static(
                pre_path,
                qdq_path,
                reader,
                quant_format=QuantFormat.QDQ,
                per_channel=True,
                activation_type=QuantType.QUInt8,
                weight_type=QuantType.QInt8,
            )
        except Exception as e:  # noqa: BLE001 - 单个模型失败不该中断整批
            print(f"  [skip] 量化失败: {type(e).__name__}: {e}")
            if os.path.exists(qdq_path):
                os.remove(qdq_path)
            continue
        dyn_path = None
        if args.with_dynamic:
            dyn_path = os.path.join(args.out_dir, f"{name}_dyn.onnx")
            try:
                quantize_dynamic(pre_path, dyn_path, weight_type=QuantType.QInt8)
            except Exception as e:  # noqa: BLE001
                print(f"  [warn] 动态量化失败: {e}")
                dyn_path = None

        rng = np.random.default_rng(11)
        eval_samples = [rng.random(shape, dtype=np.float32) for _ in range(args.eval)]

        row = {
            "model": name,
            "shape": shape,
            "size_fp32_kb": os.path.getsize(path) / 1024,
        }
        row["size_qdq_kb"] = os.path.getsize(qdq_path) / 1024
        row.update({f"qdq_{k}": v for k, v in evaluate(path, qdq_path, in_name, eval_samples).items()})
        row["lat_fp32_ms"] = latency(path, in_name, shape)
        row["lat_qdq_ms"] = latency(qdq_path, in_name, shape)

        if dyn_path:
            row["size_dyn_kb"] = os.path.getsize(dyn_path) / 1024
            row.update({f"dyn_{k}": v for k, v in evaluate(path, dyn_path, in_name, eval_samples).items()})
            row["lat_dyn_ms"] = latency(dyn_path, in_name, shape)

        rows.append(row)
        print(
            f"  size: {row['size_fp32_kb']:.0f} KB -> qdq {row['size_qdq_kb']:.0f} KB "
            f"({row['size_fp32_kb'] / max(row['size_qdq_kb'], 1e-9):.2f}x)"
            + (f", dyn {row['size_dyn_kb']:.0f} KB" if dyn_path else "")
        )
        print(
            f"  qdq : max_abs={row['qdq_max_abs_err']:.4f} mean_abs={row['qdq_mean_abs_err']:.5f} "
            f"cos>={row['qdq_cosine_min']:.5f} top1={row['qdq_top1_agree']:.2%} "
            f"lat {row['lat_fp32_ms']:.2f} -> {row['lat_qdq_ms']:.2f} ms "
            f"({row['lat_fp32_ms'] / max(row['lat_qdq_ms'], 1e-9):.2f}x)"
        )
        if dyn_path:
            print(
                f"  dyn : max_abs={row['dyn_max_abs_err']:.4f} cos>={row['dyn_cosine_min']:.5f} "
                f"top1={row['dyn_top1_agree']:.2%} lat {row['lat_dyn_ms']:.2f} ms "
                f"({row['lat_fp32_ms'] / max(row['lat_dyn_ms'], 1e-9):.2f}x)"
            )

    summary = os.path.join(args.out_dir, "summary.json")
    with open(summary, "w", encoding="utf-8") as f:
        json.dump(rows, f, indent=2, ensure_ascii=False)
    print(f"\n汇总写入 {summary}")
    if args.calib_dir is None and args.calib_npy is None:
        print("注意：本次用合成校准数据。精度指标只能说明流程通，"
              "真实精度请用 --calib-dir 指向真实图片重跑。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
