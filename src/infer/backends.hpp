#pragma once
// =============================================================================
//  内置后端注册入口（内部头，不对外）
//
//  为什么不用"全局静态对象自注册"：
//    mylib_infer 是静态库时，若某个 .obj 里只有静态注册对象、没有其它被引用的
//    符号，链接器会整个丢掉的 —— 表现是"后端没注册"。这里改成显式调用，
//    由 infer.cpp 用 std::call_once 保证只执行一次。
// =============================================================================

namespace my::infer::detail {

void register_stub_backend();

#if defined(MYLIB_HAVE_ONNXRUNTIME)
void register_onnxruntime_backend();
#endif

#if defined(MYLIB_HAVE_NCNN)
void register_ncnn_backend();
#endif

}  // namespace my::infer::detail
