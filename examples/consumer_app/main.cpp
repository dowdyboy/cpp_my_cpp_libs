// =============================================================================
//  独立消费方示例：模拟"我自己的另一个工程"如何引用 my_cpp_libs
// =============================================================================

#include <my_cpp_libs/core/build_info.hpp>
#include <my_cpp_libs/json/json.hpp>
#include <my_cpp_libs/log/format.hpp>
#include <my_cpp_libs/log/log.hpp>

int main() {
    my::log::Config cfg;
    cfg.logger_name = "consumer";
    cfg.level       = my::log::Level::Debug;
    my::log::init(cfg);

    my::log::info("消费方工程启动，{}", my::BuildInfo::summary());

    my::json::Document doc;
    doc.set("project", "consumer_app");
    doc.set("libs[0]", "my_cpp_libs::log");
    doc.set("libs[1]", "my_cpp_libs::json");

    my::log::info("配置:\n{}", doc.dump(2));
    my::log::info("读取 project = {}", doc.get_or<std::string>("project", "?"));

    my::log::shutdown();
    return 0;
}
