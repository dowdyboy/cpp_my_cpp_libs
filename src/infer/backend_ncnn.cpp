// =============================================================================
//  ncnn 后端
//
//  定位：ONNX Runtime 的备选路线。ncnn 在 armv7 上体积小、NEON 优化好，
//  适合"模型结构比较常规的 CNN"（分类、轻量检测）。缺点是：
//    * 模型要先转成 ncnn 自己的 .param + .bin（onnx2ncnn）
//    * 遇到不支持的算子直接转换失败
//  所以默认优先级是 onnxruntime -> ncnn。
//
//  模型路径约定：
//    Options::model_path      -> .param 文件
//    provider_options["bin"]  -> .bin 文件（可省略，默认同名 .bin）
// =============================================================================

#include "my_cpp_libs/infer/infer.hpp"

#include "backends.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(MYLIB_HAVE_NCNN)

#include <ncnn/mat.h>
#include <ncnn/net.h>

namespace my::infer {
namespace {

std::string option_value(const Options& options, const char* key) {
    for (const auto& kv : options.provider_options) {
        if (kv.first == key) return kv.second;
    }
    return {};
}

/// m.param -> m.bin
std::string default_bin_path(const std::string& param_path) {
    const std::string suffix = ".param";
    if (param_path.size() > suffix.size() &&
        param_path.compare(param_path.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return param_path.substr(0, param_path.size() - suffix.size()) + ".bin";
    }
    return param_path + ".bin";
}

/// Tensor 的形状 -> ncnn::Mat 的 (w,h,c)
/// ncnn 没有 batch 维，[1,C,H,W] 视为一张图
Status shape_to_wch(const Shape& shape, int& w, int& h, int& c) {
    const auto& d = shape.dims;
    switch (d.size()) {
        case 1:
            w = static_cast<int>(d[0]);
            h = 1;
            c = 1;
            return Ok();
        case 2:
            w = static_cast<int>(d[1]);
            h = 1;
            c = static_cast<int>(d[0]);
            return Ok();
        case 3:
            w = static_cast<int>(d[2]);
            h = static_cast<int>(d[1]);
            c = static_cast<int>(d[0]);
            return Ok();
        case 4:
            if (d[0] != 1) {
                return Status(Errc::Unsupported,
                              "ncnn 后端不支持 batch>1（收到 " + shape.to_string() + "）");
            }
            w = static_cast<int>(d[3]);
            h = static_cast<int>(d[2]);
            c = static_cast<int>(d[1]);
            return Ok();
        default:
            return Status(Errc::Unsupported, "ncnn 后端不支持的张量维数: " + shape.to_string());
    }
}

Shape shape_from_mat(const ncnn::Mat& m) {
    // 按实际尺寸归一化：ncnn 的 dims 有时是 3 但 h/c 都是 1（例如 Gemm 输出）
    if (m.c > 1) {
        return Shape{1, static_cast<std::int64_t>(m.c), static_cast<std::int64_t>(m.h),
                     static_cast<std::int64_t>(m.w)};
    }
    if (m.h > 1) {
        return Shape{1, static_cast<std::int64_t>(m.h), static_cast<std::int64_t>(m.w)};
    }
    return Shape{1, static_cast<std::int64_t>(m.w)};
}

class NcnnSession final : public Session {
public:
    explicit NcnnSession(const Options& options) {
        param_path_ = options.model_path;
        bin_path_   = option_value(options, "bin");
        if (bin_path_.empty()) bin_path_ = default_bin_path(param_path_);

        net_.opt.num_threads = std::max(1, options.intra_threads);
        net_.opt.use_vulkan_compute = false;
        // 关掉 fp16：输出保持 f32，跨平台数值行为更可预测
        net_.opt.use_fp16_packed     = false;
        net_.opt.use_fp16_storage    = false;
        net_.opt.use_fp16_arithmetic = false;

        if (net_.load_param(param_path_.c_str()) != 0) {
            throw std::runtime_error("load_param 失败: " + param_path_);
        }
        if (net_.load_model(bin_path_.c_str()) != 0) {
            throw std::runtime_error("load_model 失败: " + bin_path_);
        }

        for (const char* n : net_.input_names()) {
            std::string name = n ? n : "";
            in_ports_.push_back(Port{name, DType::Float32, Shape{}});
            in_names_.push_back(std::move(name));
        }
        for (const char* n : net_.output_names()) {
            std::string name = n ? n : "";
            out_ports_.push_back(Port{name, DType::Float32, Shape{}});
            out_names_.push_back(std::move(name));
        }

        if (in_ports_.empty()) throw std::runtime_error("模型没有输入 blob");
        if (out_ports_.empty()) throw std::runtime_error("模型没有输出 blob");
    }

    const char* backend_name() const noexcept override { return "ncnn"; }

    const std::vector<Port>& inputs() const noexcept override { return in_ports_; }
    const std::vector<Port>& outputs() const noexcept override { return out_ports_; }

    Status run(const std::vector<Feed>& feeds, std::vector<Outcome>& outcomes) override {
        try {
            if (feeds.size() != in_ports_.size()) {
                return Status(Errc::InvalidArgument,
                              "ncnn: 需要 " + std::to_string(in_ports_.size()) + " 个输入，收到 " +
                                  std::to_string(feeds.size()));
            }

            ncnn::Extractor ex = net_.create_extractor();

            for (std::size_t i = 0; i < in_ports_.size(); ++i) {
                const Tensor* t = nullptr;
                for (const auto& f : feeds) {
                    if (f.name == in_names_[i]) {
                        t = &f.tensor;
                        break;
                    }
                }
                if (t == nullptr && feeds.size() == in_ports_.size()) {
                    t = &feeds[i].tensor;  // 名字对不上时按顺序喂
                }
                if (t == nullptr) {
                    return Status(Errc::InvalidArgument, "ncnn: 缺少输入 " + in_names_[i]);
                }
                if (t->type != DType::Float32) {
                    return Status(Errc::Unsupported, std::string("ncnn 后端只接受 f32 输入，收到 ") +
                                                         dtype_name(t->type));
                }

                int w = 0, h = 0, c = 0;
                if (auto st = shape_to_wch(t->shape, w, h, c); !st) return st;

                // 关键：拷贝到 ncnn 自己分配的内存。
                // 两个原因：
                //   1) ncnn 的 SIMD kernel 假定 32/64 字节对齐，std::vector 只有 16 字节；
                //   2) ncnn 的通道步长 cstep 会按 16 字节对齐补齐，不能当成紧凑布局直接 memcpy。
                ncnn::Mat in(w, h, c);
                if (in.empty()) return Status(Errc::RuntimeError, "ncnn: 分配输入 Mat 失败");

                const std::size_t plane = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
                const std::size_t need  = plane * static_cast<std::size_t>(c) * sizeof(float);
                if (need > t->byte_size()) {
                    return Status(Errc::InvalidArgument, "ncnn: 输入缓冲区小于形状所需");
                }
                const float* src = static_cast<const float*>(t->data);
                if (c == 1) {
                    std::memcpy(in.data, src, plane * sizeof(float));
                } else {
                    for (int q = 0; q < c; ++q) {
                        ncnn::Mat ch = in.channel(q);
                        std::memcpy(ch.data, src + static_cast<std::size_t>(q) * plane,
                                    plane * sizeof(float));
                    }
                }

                if (ex.input(in_names_[i].c_str(), in) != 0) {
                    return Status(Errc::RuntimeError, "ncnn: 设置输入失败 " + in_names_[i]);
                }
            }

            out_mats_.clear();
            out_buffers_.clear();
            out_mats_.reserve(out_names_.size());
            out_buffers_.reserve(out_names_.size());

            for (std::size_t i = 0; i < out_names_.size(); ++i) {
                ncnn::Mat raw;
                if (ex.extract(out_names_[i].c_str(), raw) != 0) {
                    return Status(Errc::RuntimeError, "ncnn: 取输出失败 " + out_names_[i]);
                }
                // 输出可能是打包布局（elempack=4/8），统一拆回 1
                if (raw.elempack != 1) {
                    ncnn::Mat plain;
                    ncnn::convert_packing(raw, plain, 1);
                    out_mats_.push_back(std::move(plain));
                } else {
                    out_mats_.push_back(std::move(raw));
                }

                // ncnn 的通道步长按 16 字节对齐补齐，不能当紧凑数组直接用。
                // 输出通常不大，这里统一重排一份紧凑的 float 缓冲交给调用方。
                const ncnn::Mat& m = out_mats_.back();
                const std::size_t plane = static_cast<std::size_t>(m.w) * static_cast<std::size_t>(m.h);
                const std::size_t total = plane * static_cast<std::size_t>(m.c);

                std::vector<float> buf(total);
                if (m.c <= 1) {
                    std::memcpy(buf.data(), m.data, total * sizeof(float));
                } else {
                    for (int q = 0; q < m.c; ++q) {
                        ncnn::Mat ch = m.channel(q);
                        std::memcpy(buf.data() + static_cast<std::size_t>(q) * plane, ch.data,
                                    plane * sizeof(float));
                    }
                }
                out_buffers_.push_back(std::move(buf));
            }

            outcomes.clear();
            outcomes.reserve(out_buffers_.size());
            for (std::size_t i = 0; i < out_buffers_.size(); ++i) {
                Outcome o;
                o.name         = out_ports_[i].name;
                o.tensor.type  = DType::Float32;
                o.tensor.shape = shape_from_mat(out_mats_[i]);
                o.tensor.data  = out_buffers_[i].data();
                o.tensor.bytes = out_buffers_[i].size() * sizeof(float);
                outcomes.push_back(std::move(o));
            }
            return Ok();
        } catch (const std::exception& e) {
            return Status(Errc::RuntimeError, std::string("ncnn: ") + e.what());
        }
    }

private:
    ncnn::Net                net_;
    std::string              param_path_;
    std::string              bin_path_;
    std::vector<std::string> in_names_;
    std::vector<std::string> out_names_;
    std::vector<Port>        in_ports_;
    std::vector<Port>        out_ports_;
    std::vector<ncnn::Mat>       out_mats_;     // 输出 Mat 的持有者
    std::vector<std::vector<float>> out_buffers_;  // 重排后的紧凑输出（交给调用方）
};

Result<std::unique_ptr<Session>> make_ncnn(const Options& options) {
    if (options.model_path.empty()) {
        return Status(Errc::InvalidArgument,
                      "ncnn 后端需要 model_path（.param 文件）；.bin 用 provider_options[\"bin\"] 指定");
    }
    try {
        return std::unique_ptr<Session>(new NcnnSession(options));
    } catch (const std::exception& e) {
        return Status(Errc::RuntimeError, std::string("加载 ncnn 模型失败: ") + e.what());
    }
}

}  // namespace

void detail::register_ncnn_backend() { register_backend("ncnn", make_ncnn); }

}  // namespace my::infer

#endif  // MYLIB_HAVE_NCNN
