#!/usr/bin/env python3
"""把 sklearn 训练出来的模型导出成 my_cpp_libs 能直接用的格式。

产出两个文件：

    model.onnx   推理图（skl2onnx 导出，opset 固定 18，zipmap 关闭）
    model.json   元数据 sidecar（特征名、类别名、输入输出名、版本、sha256）

为什么是这两个文件：
  * ONNX 走板端已有的 ONNX Runtime 后端（my::infer），C++ 侧不需要新依赖；
  * sidecar 解决"特征顺序/类别名"这类**错了也不报错**的问题 ——
    模型只认 float 数组，特征顺序错一位输出就是垃圾，所以要把顺序写下来，
    由 my::ml 在运行时校验。

用法：
    python tools/sklearn_export.py --model clf.pkl --out models/ml/iris_logreg.onnx
    python tools/sklearn_export.py --model clf.pkl --out x.onnx --feature-names a,b,c
    python tools/sklearn_export.py --model clf.pkl --out x.onnx --no-check

也可以当库用（tools/train_ml_models.py 就是这么用的）：
    from sklearn_export import export_model
    meta = export_model(model, "out.onnx")
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pickle
import sys
from datetime import datetime, timezone

import numpy as np

try:  # joblib 是 sklearn 的依赖，正常都在
    import joblib
except Exception:  # pragma: no cover
    joblib = None

import sklearn
from sklearn.base import is_classifier, is_regressor
from sklearn.pipeline import Pipeline
from sklearn.preprocessing import MinMaxScaler, StandardScaler

import onnx
import onnxruntime as ort
from skl2onnx import convert_sklearn
from skl2onnx.common.data_types import FloatTensorType

FORMAT = "my_cpp_libs.ml/1"
DEFAULT_OPSET = 18  # 板端 ORT 1.18.1 的保守上限


# ----------------------------------------------------------------------------- 模型加载
def load_model(path: str):
    """支持 .pkl / .joblib / .sav。"""
    if joblib is not None:
        try:
            return joblib.load(path)
        except Exception:
            pass
    with open(path, "rb") as f:
        return pickle.load(f)


# ----------------------------------------------------------------------------- 元数据
def _n_features(model) -> int:
    n = getattr(model, "n_features_in_", None)
    if n is None:
        est = model.steps[-1][1] if isinstance(model, Pipeline) else model
        coef = getattr(est, "coef_", None)
        if coef is not None and np.ndim(coef) == 2:
            n = coef.shape[1]
    if n is None:
        raise SystemExit("无法确定特征数：请给 --feature-names，或换成 sklearn 1.0+ 训练的模型")
    return int(n)


def _feature_names(model, given) -> list[str]:
    if given:
        return list(given)
    names = getattr(model, "feature_names_in_", None)
    if names is not None:
        return [str(x) for x in names]
    return [f"f{i}" for i in range(_n_features(model))]


def _preprocess_info(model) -> dict:
    """记录顺序预处理器的参数（仅作参考/未来原生路径用；ONNX 里已经烘进去了）。"""
    if not isinstance(model, Pipeline):
        return {"type": "none", "embedded_in_onnx": False}
    first = model.steps[0][1] if model.steps else None
    if isinstance(first, StandardScaler):
        return {
            "type": "standard",
            "embedded_in_onnx": True,
            "mean": [float(v) for v in first.mean_],
            "scale": [float(v) for v in first.scale_],
        }
    if isinstance(first, MinMaxScaler):
        return {
            "type": "minmax",
            "embedded_in_onnx": True,
            "data_min": [float(v) for v in first.data_min_],
            "data_range": [float(v) for v in first.data_range_],
        }
    return {"type": "other", "embedded_in_onnx": True}


def _sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest().upper()


# ----------------------------------------------------------------------------- 导出
def export_model(model, out_onnx: str, *, target_opset: int = DEFAULT_OPSET,
                 feature_names=None, class_labels=None, sidecar: bool = True,
                 check: bool = True, check_samples: int = 256,
                 check_input: np.ndarray | None = None) -> dict:
    """导出 ONNX + sidecar，返回元数据 dict。"""
    n_features = _n_features(model)
    names = _feature_names(model, feature_names)
    if len(names) != n_features:
        raise SystemExit(f"特征名数量 {len(names)} 与特征数 {n_features} 不匹配")

    is_clf = is_classifier(model)
    is_reg = is_regressor(model)
    if not (is_clf or is_reg):
        raise SystemExit("既不是分类器也不是回归器，不支持")

    options = {"zipmap": False} if is_clf else {}
    onnx_model = convert_sklearn(
        model,
        initial_types=[("input", FloatTensorType([None, n_features]))],
        target_opset=target_opset,
        options=options,
    )
    onnx.checker.check_model(onnx_model)

    os.makedirs(os.path.dirname(os.path.abspath(out_onnx)), exist_ok=True)
    with open(out_onnx, "wb") as f:
        f.write(onnx_model.SerializeToString())

    # ---- 从图里读真实的输入/输出名，别在 C++ 侧猜 ----
    g = onnx_model.graph
    outputs = []
    for o in g.output:
        dims = [d.dim_value if d.HasField("dim_value") else -1
                for d in o.type.tensor_type.shape.dim]
        outputs.append({
            "name": o.name,
            "dtype": onnx.TensorProto.DataType.Name(o.type.tensor_type.elem_type).lower(),
            "dims": dims,
        })
    input_dims = [d.dim_value if d.HasField("dim_value") else -1
                  for d in g.input[0].type.tensor_type.shape.dim]

    classes = []
    class_ids = []
    if is_clf and hasattr(model, "classes_"):
        classes = [str(c) for c in (class_labels if class_labels is not None else model.classes_)]
        class_ids = [int(c) if np.issubdtype(np.asarray(model.classes_).dtype, np.integer) else i
                     for i, c in enumerate(model.classes_)]

    meta = {
        "format": FORMAT,
        "kind": "classifier" if is_clf else "regressor",
        "model": {
            "onnx": os.path.basename(out_onnx),
            "sha256": _sha256(out_onnx),
            "opset": {o.domain or "ai.onnx": o.version for o in onnx_model.opset_import},
        },
        "input": {
            "name": g.input[0].name,
            "dtype": "float32",
            "n_features": n_features,
            "dims": input_dims,
            "features": names,
        },
        "output": {
            "items": outputs,
            "n_classes": len(classes),
            "classes": classes,
            "class_ids": class_ids,
        },
        "preprocess": _preprocess_info(model),
        "training": {
            "sklearn": sklearn.__version__,
            "onnx": onnx.__version__,
            "onnxruntime": ort.__version__,
            "python": sys.version.split()[0],
            "exported_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        },
    }

    # ---- 自检：sklearn 与 ONNX 的预测必须对得上 ----
    if check:
        rng = np.random.default_rng(0)
        X = check_input if check_input is not None else rng.normal(0.0, 1.0, (check_samples, n_features))
        X = np.asarray(X, dtype=np.float32)
        sess = ort.InferenceSession(out_onnx, providers=["CPUExecutionProvider"])
        got = sess.run(None, {g.input[0].name: X})
        if is_clf:
            prob = None
            for arr in got:
                if isinstance(arr, np.ndarray) and arr.ndim == 2 and arr.shape[0] == X.shape[0]:
                    prob = arr
            ref = model.predict_proba(X.astype(np.float64))
            diff = float(np.max(np.abs(prob - ref)))
            agree = float(np.mean(np.argmax(prob, 1) == model.predict(X.astype(np.float64))))
            meta["check"] = {"max_abs_prob_diff": diff, "argmax_agreement": agree,
                             "samples": int(X.shape[0])}
            if agree < 1.0 or diff > 1e-3:
                print(f"[warn] 自检偏差偏大：max|Δp|={diff:.2e} argmax={agree:.4f}", file=sys.stderr)
        else:
            ref = model.predict(X.astype(np.float64)).reshape(-1)
            pred = got[0].reshape(-1)
            diff = float(np.max(np.abs(pred - ref)))
            meta["check"] = {"max_abs_diff": diff, "samples": int(X.shape[0])}
            if diff > 1e-3:
                print(f"[warn] 回归自检偏差偏大：max|Δ|={diff:.2e}", file=sys.stderr)

    if sidecar:
        json_path = os.path.splitext(out_onnx)[0] + ".json"
        with open(json_path, "w", encoding="utf-8") as f:
            json.dump(meta, f, ensure_ascii=False, indent=2)
            f.write("\n")
    return meta


# ----------------------------------------------------------------------------- CLI
def main(argv=None):
    ap = argparse.ArgumentParser(description="sklearn -> ONNX(+sidecar)，给 my_cpp_libs 用")
    ap.add_argument("--model", required=True, help="训练好的模型（.pkl/.joblib）")
    ap.add_argument("--out", required=True, help="输出 .onnx 路径")
    ap.add_argument("--opset", type=int, default=DEFAULT_OPSET, help=f"目标 opset（默认 {DEFAULT_OPSET}）")
    ap.add_argument("--feature-names", default=None, help="逗号分隔；不填则用模型里的 feature_names_in_ 或 f0..fN")
    ap.add_argument("--class-labels", default=None, help="逗号分隔的类别显示名（默认用 model.classes_）")
    ap.add_argument("--no-sidecar", action="store_true", help="不写 .json")
    ap.add_argument("--no-check", action="store_true", help="跳过 sklearn/ONNX 数值对拍")
    args = ap.parse_args(argv)

    model = load_model(args.model)
    names = args.feature_names.split(",") if args.feature_names else None
    clabels = args.class_labels.split(",") if args.class_labels else None
    meta = export_model(model, args.out, target_opset=args.opset, feature_names=names,
                        class_labels=clabels,
                        sidecar=not args.no_sidecar, check=not args.no_check)

    kind = meta["kind"]
    n_in = meta["input"]["n_features"]
    out_items = meta["output"]["items"]
    print(f"[ok] {args.out}")
    print(f"     kind={kind}  n_features={n_in}  opset={meta['model']['opset']}")
    print(f"     输入: {meta['input']['name']}  float32 [?,{n_in}]")
    for o in out_items:
        print(f"     输出: {o['name']:16} {o['dtype']:8} {o['dims']}")
    if kind == "classifier":
        print(f"     类别: {meta['output']['classes']}")
    if "check" in meta:
        c = meta["check"]
        if kind == "classifier":
            print(f"     自检: max|Δp|={c['max_abs_prob_diff']:.2e}  argmax一致={c['argmax_agreement']:.3f}")
        else:
            print(f"     自检: max|Δ|={c['max_abs_diff']:.2e}")
    print(f"     sidecar: {os.path.splitext(args.out)[0]}.json" if not args.no_sidecar else "     sidecar: 未写")
    return 0


if __name__ == "__main__":
    sys.exit(main())
