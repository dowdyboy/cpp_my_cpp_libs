#include "my_cpp_libs/infer/infer.hpp"

#include "backends.hpp"

#include <cstring>
#include <memory>
#include <vector>

namespace my::infer {
namespace {

// =============================================================================
//  stub 后端：把输入原样回显
//
//  用途
//    * 在还没有真模型的阶段先把"采集 -> 预处理 -> 推理 -> 后处理"链路搭起来
//    * 在没有 ONNX Runtime 的平台（如 Zynq-7000）上让业务代码照常运行
// =============================================================================
class StubSession final : public Session {
public:
    StubSession() {
        inputs_.push_back(Port{"input", DType::Float32, Shape{-1}});
        outputs_.push_back(Port{"output", DType::Unknown, Shape{}});
    }

    const char* backend_name() const noexcept override { return "stub"; }

    const std::vector<Port>& inputs() const noexcept override { return inputs_; }
    const std::vector<Port>& outputs() const noexcept override { return outputs_; }

    Status run(const std::vector<Feed>& feeds, std::vector<Outcome>& outcomes) override {
        if (feeds.size() != 1) {
            return Status(Errc::InvalidArgument,
                          "stub 后端需要恰好 1 个输入，实际收到 " + std::to_string(feeds.size()));
        }
        const Tensor& in = feeds[0].tensor;
        if (in.data == nullptr) {
            return Status(Errc::InvalidArgument, "stub 后端收到空输入缓冲区");
        }
        const std::size_t n = in.byte_size();
        if (n == 0) {
            return Status(Errc::InvalidArgument, "stub 后端输入字节数为 0（形状可能含动态维）");
        }

        buffer_.resize(n);
        std::memcpy(buffer_.data(), in.data, n);

        outputs_[0].type  = in.type;
        outputs_[0].shape = in.shape;

        outcomes.clear();
        outcomes.reserve(1);
        Outcome o;
        o.name   = outputs_[0].name;
        o.tensor = Tensor(in.type, in.shape, buffer_.data(), n);
        outcomes.push_back(std::move(o));
        return Ok();
    }

private:
    std::vector<Port>         inputs_;
    std::vector<Port>         outputs_;
    std::vector<std::uint8_t> buffer_;
};

Result<std::unique_ptr<Session>> make_stub(const Options&) {
    return std::unique_ptr<Session>(new StubSession());
}

}  // namespace

// 显式注册（见 backends.hpp 说明）
void detail::register_stub_backend() { register_backend("stub", make_stub); }

}  // namespace my::infer
