// =============================================================================
//  示例 02：JSON 配置
//  演示路径式访问、结构化绑定（NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE）、
//  文件读写、错误处理。
// =============================================================================

#include <my_cpp_libs/json/json.hpp>
#include <my_cpp_libs/log/format.hpp>
#include <my_cpp_libs/log/log.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

// -----------------------------------------------------------------------------
//  1) 定义业务结构体，交给 nlohmann 做映射
// -----------------------------------------------------------------------------
struct CameraConfig {
    std::string name = "cam0";
    int         width = 1280;
    int         height = 720;
    double      fps = 30.0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CameraConfig, name, width, height, fps)

struct AppConfig {
    std::string              app_name = "demo";
    CameraConfig             camera;
    std::vector<std::string> devices{"imu0", "lidar0"};
    bool                     debug = false;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AppConfig, app_name, camera, devices, debug)

int main() {
    my::log::Config lc;
    lc.level = my::log::Level::Debug;
    if (auto st = my::log::init(lc); !st) {
        std::printf("日志初始化失败: %s\n", st.to_string().c_str());
        return 1;
    }

    // -------------------------------------------------------------------------
    //  2) 手工构造 + 路径写入（自动创建中间节点）
    // -------------------------------------------------------------------------
    my::json::Document doc;
    (void)doc.set("app_name", "my_cpp_libs demo");
    (void)doc.set("camera.name", "cam_front");
    (void)doc.set("camera.width", 1920);
    (void)doc.set("camera.height", 1080);
    (void)doc.set("camera.fps", 60.0);
    (void)doc.set("devices[0]", "imu0");
    (void)doc.set("devices[1]", "lidar0");
    (void)doc.set("network.host", "127.0.0.1");
    (void)doc.set("network.port", 9000);

    my::log::info("生成的 JSON:\n{}", doc.dump(2));

    // -------------------------------------------------------------------------
    //  3) 路径读取：带类型检查，不抛异常
    // -------------------------------------------------------------------------
    auto width = doc.get<int>("camera.width");
    if (width) {
        my::log::info("camera.width = {}", width.value());
    }

    // 类型不匹配 -> 明确的错误，而不是崩溃
    auto as_string = doc.get<std::string>("camera.width");
    if (!as_string) {
        my::log::warn("预期失败: {}", as_string.status().to_string());
    }

    // 不存在 -> NotFound
    auto missing = doc.get<int>("camera.exposure");
    if (!missing) {
        my::log::warn("预期失败: {}", missing.status().to_string());
    }

    // 带默认值
    const int exposure = doc.get_or<int>("camera.exposure", -1);
    my::log::info("camera.exposure 缺省值 = {}", exposure);

    // -------------------------------------------------------------------------
    //  4) 落盘 + 重新加载
    // -------------------------------------------------------------------------
    std::error_code ec;
    std::filesystem::create_directories("output", ec);
    const std::string path = "output/app_config.json";
    if (auto st = doc.save_file(path, 2); !st) {
        my::log::error("保存失败: {}", st.to_string());
        return 1;
    }

    auto loaded = my::json::Document::parse_file(path);
    if (!loaded) {
        my::log::error("加载失败: {}", loaded.status().to_string());
        return 1;
    }

    // -------------------------------------------------------------------------
    //  5) 结构化绑定：整块取成 C++ 结构体
    // -------------------------------------------------------------------------
    CameraConfig cam = loaded.value().get<CameraConfig>("camera").value_or(CameraConfig{});
    my::log::info("相机: {} {}x{} @{}fps", cam.name, cam.width, cam.height, cam.fps);

    // 反向：结构体 -> 文档
    AppConfig app;
    app.app_name = "structured";
    app.camera   = cam;
    app.debug    = true;

    my::json::Document from_struct{my::json::Json(app)};
    my::log::info("结构体转回 JSON:\n{}", from_struct.dump());
    my::log::info("keys = {}", from_struct.keys().size());
    my::log::info("devices 数量 = {}", from_struct.size("devices"));

    // 删除字段
    (void)loaded.value().erase("network.port");
    my::log::info("删除 network.port 后 keys(network) = {}",
                  loaded.value().keys("network").size());

    my::log::shutdown();
    return 0;
}
