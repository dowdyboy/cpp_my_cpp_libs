# 兜底许可证

`cmake/Packaging.cmake` 在打包时要求每个随包三方依赖都有一份许可证；缺任何一份会
`FATAL_ERROR`，不再只打印 WARNING。

这里放的是**自编依赖**没带 LICENSE 时的兜底副本：

- `onnxruntime-LICENSE`：ONNX Runtime 的 MIT 许可证（1.19.2 官方包中的原文）；
- `onnxruntime-ThirdPartyNotices.txt`：ONNX Runtime 的第三方声明（同上）。

armv7 的 `third_party/onnxruntime-armv7-petalinux2020/` 是本机自编产物，目录里只有
`README.txt`/`VERSION_NUMBER`，没有这两份文件；`Packaging.cmake` 会在 ORT 根目录找不到
许可证时自动使用本目录的副本。换 ORT 版本时请同步更新这两份文本。
