#include "my_cpp_libs/core/status.hpp"

namespace my {

const char* errc_name(Errc code) noexcept {
    switch (code) {
        case Errc::Ok: return "ok";
        case Errc::InvalidArgument: return "invalid_argument";
        case Errc::OutOfRange: return "out_of_range";
        case Errc::NotFound: return "not_found";
        case Errc::AlreadyExists: return "already_exists";
        case Errc::IoError: return "io_error";
        case Errc::ParseError: return "parse_error";
        case Errc::Unsupported: return "unsupported";
        case Errc::NotInitialized: return "not_initialized";
        case Errc::Busy: return "busy";
        case Errc::RuntimeError: return "runtime_error";
        case Errc::Cancelled: return "cancelled";
        case Errc::Timeout: return "timeout";
        case Errc::Unknown: return "unknown";
    }
    return "unrecognized";
}

std::string Status::to_string() const {
    if (ok()) return "ok";
    std::string out = errc_name(code_);
    if (!message_.empty()) {
        out += ": ";
        out += message_;
    }
    return out;
}

}  // namespace my
