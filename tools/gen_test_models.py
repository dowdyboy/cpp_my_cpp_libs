#!/usr/bin/env python3
# =============================================================================
#  生成一组"形态各异"的测试模型，用来验证 my::infer 对各类模型的兼容性
#
#  覆盖点：
#    * 动态 batch 维（-1）
#    * 多输入 / 多输出
#    * int64 输入（NLP 的 token id 形态）
#    * fp16 输入输出
#    * uint8 图像输入
#    * 真实卷积网络（测算子覆盖 + 延迟）
#
#  用法（需要 onnx + numpy）：
#      python tools/gen_test_models.py [输出目录]
# =============================================================================

import os
import sys

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper


OUT_DIR = sys.argv[1] if len(sys.argv) > 1 else "models"


def _save(model: onnx.ModelProto, name: str) -> None:
    onnx.checker.check_model(model)
    path = os.path.join(OUT_DIR, name)
    onnx.save(model, path)
    print(f"  {name:32s} {os.path.getsize(path):>8d} bytes")


def _finish(nodes, inputs, outputs, initializers=(), opset: int = 17, name="g"):
    graph = helper.make_graph(nodes, name, inputs, outputs, list(initializers))
    model = helper.make_model(
        graph,
        producer_name="my_cpp_libs test generator",
        opset_imports=[helper.make_opsetid("", opset)],
    )
    model.ir_version = 8
    return model


def make_dynamic_batch():
    """[N,4] * W[4,2] + b[2] -> [N,2]，N 为动态维"""
    W = numpy_helper.from_array(np.arange(8, dtype=np.float32).reshape(4, 2) * 0.1, "W")
    b = numpy_helper.from_array(np.array([0.5, -0.5], dtype=np.float32), "b")

    inp = helper.make_tensor_value_info("x", TensorProto.FLOAT, ["N", 4])
    out = helper.make_tensor_value_info("y", TensorProto.FLOAT, ["N", 2])

    nodes = [
        helper.make_node("MatMul", ["x", "W"], ["mm"], name="mm"),
        helper.make_node("Add", ["mm", "b"], ["y"], name="add"),
    ]
    return _finish(nodes, [inp], [out], [W, b], name="dynamic_batch")


def make_multi_io():
    """两进两出：sum 与 diff"""
    a = helper.make_tensor_value_info("a", TensorProto.FLOAT, [1, 3])
    b = helper.make_tensor_value_info("b", TensorProto.FLOAT, [1, 3])
    s = helper.make_tensor_value_info("sum", TensorProto.FLOAT, [1, 3])
    d = helper.make_tensor_value_info("diff", TensorProto.FLOAT, [1, 3])
    nodes = [
        helper.make_node("Add", ["a", "b"], ["sum"], name="add"),
        helper.make_node("Sub", ["a", "b"], ["diff"], name="sub"),
    ]
    return _finish(nodes, [a, b], [s, d], name="multi_io")


def make_int64_input():
    """int64 token ids -> cast 成 f32 再乘 0.01"""
    ids = helper.make_tensor_value_info("ids", TensorProto.INT64, [1, 8])
    out = helper.make_tensor_value_info("emb", TensorProto.FLOAT, [1, 8])
    scale = numpy_helper.from_array(np.array([0.01], dtype=np.float32), "scale")
    reshape = helper.make_tensor_value_info("cast", TensorProto.FLOAT, [1, 8])
    nodes = [
        helper.make_node("Cast", ["ids"], ["cast_f"], to=TensorProto.FLOAT, name="cast"),
        helper.make_node("Mul", ["cast_f", "scale"], ["emb"], name="scale"),
    ]
    return _finish(nodes, [ids], [out], [scale], name="int64_input")


def make_fp16_io():
    """fp16 进 fp16 出：y = x + 1"""
    inp = helper.make_tensor_value_info("x", TensorProto.FLOAT16, [1, 4])
    out = helper.make_tensor_value_info("y", TensorProto.FLOAT16, [1, 4])
    one = numpy_helper.from_array(np.array([1.0], dtype=np.float16), "one")
    nodes = [helper.make_node("Add", ["x", "one"], ["y"], name="add")]
    return _finish(nodes, [inp], [out], [one], name="fp16_io")


def make_u8_input():
    """uint8 图像 -> 归一化到 f32"""
    inp = helper.make_tensor_value_info("image", TensorProto.UINT8, [1, 3, 8, 8])
    out = helper.make_tensor_value_info("norm", TensorProto.FLOAT, [1, 3, 8, 8])
    scale = numpy_helper.from_array(np.array([1.0 / 255.0], dtype=np.float32), "scale")
    nodes = [
        helper.make_node("Cast", ["image"], ["as_float"], to=TensorProto.FLOAT, name="cast"),
        helper.make_node("Mul", ["as_float", "scale"], ["norm"], name="scale"),
    ]
    return _finish(nodes, [inp], [out], [scale], name="u8_input")


def make_small_cnn():
    """Conv-BN-ReLU-Pool x2 -> GAP -> Gemm -> [1,10]，用来测延迟"""
    inp = helper.make_tensor_value_info("input", TensorProto.FLOAT, [1, 3, 32, 32])
    out = helper.make_tensor_value_info("logits", TensorProto.FLOAT, [1, 10])

    rng = np.random.default_rng(0)
    inits = []
    nodes = []
    channels = [(3, 16), (16, 32)]

    prev = "input"
    for i, (cin, cout) in enumerate(channels):
        w = rng.normal(0, 0.1, size=(cout, cin, 3, 3)).astype(np.float32)
        b = np.zeros(cout, dtype=np.float32)
        inits.append(numpy_helper.from_array(w, f"w{i}"))
        inits.append(numpy_helper.from_array(b, f"b{i}"))
        # kernel_shape 必须显式写出来：ONNX 里它是可选的（可由权重推断），
        # 但 onnx2ncnn 读的是这个属性，缺了会退化成 kernel=0，
        # 之后 ncnn 推理直接整数除零崩溃。
        nodes.append(helper.make_node("Conv", [prev, f"w{i}", f"b{i}"], [f"conv{i}"],
                                      name=f"conv{i}", pads=[1, 1, 1, 1],
                                      kernel_shape=[3, 3]))
        nodes.append(helper.make_node("Relu", [f"conv{i}"], [f"relu{i}"], name=f"relu{i}"))
        nodes.append(helper.make_node("MaxPool", [f"relu{i}"], [f"pool{i}"], name=f"pool{i}",
                                      kernel_shape=[2, 2], strides=[2, 2]))
        prev = f"pool{i}"

    nodes.append(helper.make_node("GlobalAveragePool", [prev], ["gap"], name="gap"))
    nodes.append(helper.make_node("Flatten", ["gap"], ["flat"], name="flatten", axis=1))
    wfc = rng.normal(0, 0.1, size=(10, 32)).astype(np.float32)
    bfc = np.zeros(10, dtype=np.float32)
    inits.append(numpy_helper.from_array(wfc, "wfc"))
    inits.append(numpy_helper.from_array(bfc, "bfc"))
    # W 按 [out, in] 存，所以需要 transB=1：logits = flat[1,32] * W^T[32,10] + b[10]
    nodes.append(helper.make_node("Gemm", ["flat", "wfc", "bfc"], ["logits"], name="fc", transB=1))

    return _finish(nodes, [inp], [out], inits, name="small_cnn")



def make_bench_cnn():
    """接近真实规模的分类网络：224x224 输入，约 300 MMAC（和 MobileNetV2 同量级）

    用途：在 Zynq 这种小算力板子上估算"真模型要跑多久"，
    比 32x32 的 small_cnn 更有参考价值。
    MobileNetV2-224 约 300 MMAC。
    """
    inp = helper.make_tensor_value_info("input", TensorProto.FLOAT, [1, 3, 224, 224])
    out = helper.make_tensor_value_info("logits", TensorProto.FLOAT, [1, 10])

    rng = np.random.default_rng(1)
    inits, nodes = [], []

    # (输入通道, 输出通道, stride)
    stages = [(3, 32, 1), (32, 32, 2), (32, 64, 2), (64, 64, 2), (64, 128, 2)]
    prev = "input"
    for i, (cin, cout, stride) in enumerate(stages):
        w = rng.normal(0, 0.05, size=(cout, cin, 3, 3)).astype(np.float32)
        b = np.zeros(cout, dtype=np.float32)
        inits += [numpy_helper.from_array(w, f"w{i}"), numpy_helper.from_array(b, f"b{i}")]
        nodes.append(helper.make_node("Conv", [prev, f"w{i}", f"b{i}"], [f"conv{i}"],
                                      name=f"conv{i}", pads=[1, 1, 1, 1],
                                      strides=[stride, stride], kernel_shape=[3, 3]))
        nodes.append(helper.make_node("Relu", [f"conv{i}"], [f"relu{i}"], name=f"relu{i}"))
        prev = f"relu{i}"

    nodes.append(helper.make_node("GlobalAveragePool", [prev], ["gap"], name="gap"))
    nodes.append(helper.make_node("Flatten", ["gap"], ["flat"], name="flatten", axis=1))
    wfc = rng.normal(0, 0.1, size=(10, 128)).astype(np.float32)
    bfc = np.zeros(10, dtype=np.float32)
    inits += [numpy_helper.from_array(wfc, "wfc"), numpy_helper.from_array(bfc, "bfc")]
    nodes.append(helper.make_node("Gemm", ["flat", "wfc", "bfc"], ["logits"], name="fc", transB=1))

    return _finish(nodes, [inp], [out], inits, name="bench_cnn")

def make_scalar_math():
    """标量/一维输入的边界情况：x[4] -> softmax"""
    inp = helper.make_tensor_value_info("x", TensorProto.FLOAT, [4])
    out = helper.make_tensor_value_info("p", TensorProto.FLOAT, [4])
    nodes = [helper.make_node("Softmax", ["x"], ["p"], name="softmax", axis=0)]
    return _finish(nodes, [inp], [out], name="softmax_1d")


def main() -> int:
    os.makedirs(OUT_DIR, exist_ok=True)
    print(f"生成测试模型到 {os.path.abspath(OUT_DIR)}:")
    _save(make_dynamic_batch(), "dyn_batch.onnx")
    _save(make_multi_io(), "multi_io.onnx")
    _save(make_int64_input(), "int64_input.onnx")
    _save(make_fp16_io(), "fp16_io.onnx")
    _save(make_u8_input(), "u8_input.onnx")
    _save(make_small_cnn(), "small_cnn.onnx")
    _save(make_bench_cnn(), "bench_cnn.onnx")
    _save(make_scalar_math(), "softmax_1d.onnx")
    print("完成")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
