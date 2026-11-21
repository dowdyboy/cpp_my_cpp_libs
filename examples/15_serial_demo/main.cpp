// =============================================================================
//  串口示例：枚举 -> 打开 -> 一次带超时的读 -> 关闭
//
//  为什么只读不写：example 不应该向一个不知道是什么的设备发数据。
//  真做协议时按 docs/13 的写法：write_all() 带总预算、read_some() 自己攒帧。
//
//  Windows：列出注册表里的 COM 口；没有虚拟串口时表为空（不是错误）。
//  Linux  ：扫 /dev/tty{USB,ACM,S,...}，并把 /dev/serial/by-id/* 作为稳定路径附上。
// =============================================================================

#include <my_cpp_libs/serial/serial.hpp>

#include <chrono>
#include <cstdio>
#include <string>

int main() {
    using namespace std::chrono_literals;

    auto ports = my::serial::list_ports();
    if (!ports) {
        std::printf("枚举串口失败: %s\n", ports.status().to_string().c_str());
        return 1;
    }

    std::printf("发现 %zu 个串口：\n", ports.value().size());
    for (const auto& p : ports.value()) {
        if (p.description.empty()) {
            std::printf("  %-24s\n", p.name.c_str());
        } else {
            std::printf("  %-24s %s\n", p.name.c_str(), p.description.c_str());
        }
    }
    if (ports.value().empty()) {
        std::printf("\n没有串口可打开。Windows 上装个虚拟串口对（com0com）再跑；\n"
                    "Linux 上插上设备或改用 PTY 测试（见 tests/test_serial.cpp）。\n");
        return 0;
    }

    // 参数校验可以先单独做：不碰设备，给配置文件用很合适
    my::serial::Config cfg;
    cfg.baud      = 115200;
    cfg.data_bits = 8;
    cfg.parity    = my::serial::Parity::None;
    cfg.stop_bits = my::serial::StopBits::One;
    cfg.flow      = my::serial::Flow::None;
    if (auto st = my::serial::validate(cfg); !st) {
        std::printf("配置非法: %s\n", st.to_string().c_str());
        return 1;
    }

    const std::string name = ports.value().front().name;
    auto port = my::serial::Port::open(name, cfg);
    if (!port) {
        // 设备存在但打不开（被占用/权限不足）是常见情况：
        // Linux 要把用户加进 dialout 组，Windows 要确认没被串口助手占着。
        std::printf("\n打开 %s 失败: %s\n", name.c_str(), port.status().to_string().c_str());
        return 1;
    }
    std::printf("\n已打开 %s（%u 8N1）\n", name.c_str(), static_cast<unsigned>(cfg.baud));

    // 只等 100 ms：超时返回 0 字节 + Ok，不是错误（docs/13 §2.1）
    unsigned char buf[64];
    auto n = port.value().read_some(buf, sizeof buf, 100ms);
    if (!n) {
        std::printf("读失败: %s\n", n.status().to_string().c_str());
    } else if (n.value() == 0) {
        std::printf("100 ms 内没有数据（正常：对端没发东西）\n");
    } else {
        std::printf("收到 %zu 字节: ", n.value());
        for (std::size_t i = 0; i < n.value(); ++i) {
            const unsigned char c = buf[i];
            std::printf("%02X ", c);
        }
        std::printf("\n");
    }

    // 析构会自动 close；显式 close 也可以，重复 close 是幂等的
    (void)port.value().close();
    std::printf("已关闭。\n");
    return 0;
}
