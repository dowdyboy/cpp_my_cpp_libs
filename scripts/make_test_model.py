#!/usr/bin/env python3
# =============================================================================
#  生成一个最小可用的 ONNX 模型（仅用标准库，不依赖 onnx / protobuf 包）
#
#  模型：y = Identity(x)，x 形状 [1, 1, 4]，float32
#  用途：在没有真实模型时验证推理链路（ONNX Runtime 后端能真正加载并运行）
#
#  用法：
#      python scripts/make_test_model.py models/identity_1x1x4.onnx
# =============================================================================

import sys
import os


# ---------------------------------------------------------------------------
#  protobuf 线格式手写编码
# ---------------------------------------------------------------------------
def _varint(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def f_varint(field: int, value: int) -> bytes:
    return _varint((field << 3) | 0) + _varint(value)


def f_bytes(field: int, data: bytes) -> bytes:
    return _varint((field << 3) | 2) + _varint(len(data)) + data


def f_string(field: int, text: str) -> bytes:
    return f_bytes(field, text.encode("utf-8"))


# ---------------------------------------------------------------------------
#  各层消息
# ---------------------------------------------------------------------------
def dimension(size: int) -> bytes:
    return f_varint(1, size)                     # Dimension.dim_value


def tensor_shape(sizes) -> bytes:
    return b"".join(f_bytes(1, dimension(s)) for s in sizes)   # TensorShapeProto.dim


def type_proto_float(sizes) -> bytes:
    tensor_type = (f_varint(1, 1) +                          # elem_type = FLOAT(1)
                   f_bytes(2, tensor_shape(sizes)))
    return f_bytes(1, tensor_type)                           # TypeProto.tensor_type


def value_info(name: str, sizes) -> bytes:
    return f_string(1, name) + f_bytes(2, type_proto_float(sizes))


def node(op_type: str, inputs, outputs, name: str = "") -> bytes:
    out = b""
    for i in inputs:
        out += f_string(1, i)                                # NodeProto.input
    for o in outputs:
        out += f_string(2, o)                                # NodeProto.output
    if name:
        out += f_string(3, name)
    out += f_string(4, op_type)                               # NodeProto.op_type
    return out


def opset_import(version: int, domain: str = "") -> bytes:
    out = b""
    if domain:
        out += f_string(1, domain)
    out += f_varint(2, version)
    return out


def build_identity_model(sizes=(1, 1, 4), opset: int = 13) -> bytes:
    graph = b""
    graph += f_bytes(1, node("Identity", ["x"], ["y"], "identity_node"))  # GraphProto.node
    graph += f_string(2, "identity_graph")                                # GraphProto.name
    graph += f_bytes(11, value_info("x", sizes))                          # GraphProto.input
    graph += f_bytes(12, value_info("y", sizes))                          # GraphProto.output

    model = b""
    model += f_varint(1, 8)                        # ModelProto.ir_version
    model += f_string(2, "my_cpp_libs")            # producer_name
    model += f_bytes(7, graph)                     # ModelProto.graph
    model += f_bytes(8, opset_import(opset))       # ModelProto.opset_import
    return model


def main() -> int:
    out_path = sys.argv[1] if len(sys.argv) > 1 else "models/identity_1x1x4.onnx"
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)

    data = build_identity_model()
    with open(out_path, "wb") as f:
        f.write(data)

    print(f"已生成 {out_path} ({len(data)} 字节)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
