#!/usr/bin/env python3
"""生成 models/ml/ 下的演示模型（可复现）。

产物（都进 git，总共约 30 KB）：
    iris_logreg.onnx / .json     逻辑回归 4 特征 3 类（线性路径）
    gbdt_20f.onnx    / .json     梯度提升 20 特征 2 类（树集成路径）
    ridge_10f.onnx   / .json     岭回归 10 特征（回归路径）
    以及每个模型对应的 .golden.tsv —— 固定输入 + sklearn 的预测结果，
    给 C++ 单元测试做离线对拍（不依赖 Python）。

为什么用固定 row 索引而不是随机采样：golden 文件要能跨机器复现，
随机数生成器版本一变就对不上。

用法：
    python tools/train_ml_models.py [--out models/ml]
"""
from __future__ import annotations

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sklearn_export import export_model  # noqa: E402

from sklearn.datasets import load_iris, make_classification, make_regression  # noqa: E402
from sklearn.ensemble import GradientBoostingClassifier  # noqa: E402
from sklearn.linear_model import LogisticRegression, Ridge  # noqa: E402

N_GOLDEN = 8


def _write_golden(path: str, X: np.ndarray, labels, probs=None) -> None:
    """文本格式：每行 `特征(空格分隔)  label  [概率...]`，方便 C++ 直接 ifstream 读。"""
    n = X.shape[1]
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"# n_features={n}\n")
        for i in range(X.shape[0]):
            feats = " ".join(f"{v:.6f}" for v in X[i])
            if probs is not None:
                ps = " ".join(f"{v:.6f}" for v in probs[i])
                f.write(f"{feats}  {int(labels[i])}  {ps}\n")
            else:
                f.write(f"{feats}  {float(labels[i]):.6f}\n")


def export_with_golden(model, out_onnx: str, X: np.ndarray, feature_names=None,
                       class_labels=None, is_clf: bool = True) -> dict:
    meta = export_model(model, out_onnx, feature_names=feature_names,
                        class_labels=class_labels, check=True)
    idx = np.linspace(0, X.shape[0] - 1, N_GOLDEN).astype(int)
    Xg = X[idx].astype(np.float64)
    if is_clf:
        _write_golden(os.path.splitext(out_onnx)[0] + ".golden.tsv", Xg,
                      model.predict(Xg), model.predict_proba(Xg))
    else:
        _write_golden(os.path.splitext(out_onnx)[0] + ".golden.tsv", Xg, model.predict(Xg))
    return meta


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join("models", "ml"))
    args = ap.parse_args(argv)
    os.makedirs(args.out, exist_ok=True)

    # ---- 1) 逻辑回归：iris ----
    iris = load_iris()
    X = iris.data.astype(np.float64)
    y = iris.target
    lr = LogisticRegression(max_iter=1000).fit(X, y)
    meta = export_with_golden(lr, os.path.join(args.out, "iris_logreg.onnx"), X,
                              feature_names=[n.replace(" (cm)", "").replace(" ", "_")
                                             for n in iris.feature_names],
                              class_labels=list(iris.target_names))
    print(f"iris_logreg     classes={meta['output']['classes']} "
          f"check={meta.get('check', {}).get('max_abs_prob_diff')}")

    # ---- 2) 梯度提升：20 特征二分类 ----
    Xb, yb = make_classification(n_samples=1000, n_features=20, n_informative=8,
                                 n_classes=2, random_state=1)
    Xb = Xb.astype(np.float64)
    gb = GradientBoostingClassifier(n_estimators=30, random_state=0).fit(Xb, yb)
    meta = export_with_golden(gb, os.path.join(args.out, "gbdt_20f.onnx"), Xb)
    print(f"gbdt_20f        classes={meta['output']['classes']} "
          f"check={meta.get('check', {}).get('max_abs_prob_diff')}")

    # ---- 3) 岭回归 ----
    Xr, yr = make_regression(n_samples=500, n_features=10, noise=0.1, random_state=2)
    Xr = Xr.astype(np.float64)
    rg = Ridge(alpha=1.0).fit(Xr, yr)
    meta = export_with_golden(rg, os.path.join(args.out, "ridge_10f.onnx"), Xr, is_clf=False)
    print(f"ridge_10f       kind={meta['kind']} check={meta.get('check', {}).get('max_abs_diff')}")

    # ---- 体积汇总 ----
    print()
    total = 0
    for name in ("iris_logreg", "gbdt_20f", "ridge_10f"):
        for ext in (".onnx", ".json", ".golden.tsv"):
            p = os.path.join(args.out, name + ext)
            if os.path.exists(p):
                total += os.path.getsize(p)
                print(f"  {name + ext:26} {os.path.getsize(p):8} B")
    print(f"  {'合计':26} {total:8} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
