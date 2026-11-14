// =============================================================================
//  示例 11：nlohmann/json 原生 API
//
//  覆盖日常最常用的 90%：构造、解析、路径访问、类型转换、容器遍历、
//  自定义类型映射、异常处理、序列化选项。
// =============================================================================

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using json = nlohmann::json;

// -----------------------------------------------------------------------------
//  自定义类型 <-> JSON（推荐用宏，字段名一目了然）
// -----------------------------------------------------------------------------
struct Point {
    double x = 0.0;
    double y = 0.0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Point, x, y)

struct Task {
    std::string        name;
    int                priority = 0;
    std::vector<Point> path;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Task, name, priority, path)

int main() {
    // =========================================================================
    //  1) 构造
    // =========================================================================
    json j;
    j["name"]    = "demo";
    j["version"] = 3;
    j["enabled"] = true;
    j["ratio"]   = 0.75;
    j["tags"]    = {"a", "b", "c"};             // 数组
    j["nested"]["level1"]["level2"] = 42;       // 自动创建嵌套对象

    json from_init = {{"a", 1}, {"b", {1, 2, 3}}, {"c", {{"d", nullptr}}}};
    std::printf("1) 构造: %s\n", j.dump().c_str());

    // =========================================================================
    //  2) 解析（C++11 原始字符串字面量最顺手）
    // =========================================================================
    const char* text = R"({
        "sensor": {"name": "imu0", "rate_hz": 200, "axes": [1.0, 0.0, 0.0]},
        "filters": ["none", "median", "kalman"]
    })";

    json parsed = json::parse(text);
    std::printf("2) 解析: sensor.name = %s\n", parsed["sensor"]["name"].get<std::string>().c_str());

    // 带异常处理的解析
    try {
        const json bad = json::parse("{ 这不是 JSON }");
        (void)bad;
    } catch (const json::parse_error& e) {
        std::printf("   解析错误被捕获: %s\n", e.what());
    }

    // =========================================================================
    //  3) 取值：get<T>() / value() / 隐式转换 / 带默认值
    // =========================================================================
    const std::string name  = parsed["sensor"]["name"];              // 隐式转换
    const int         rate  = parsed["sensor"]["rate_hz"];           // 隐式转换
    const double      first = parsed["sensor"]["axes"][0].get<double>();

    // value(key, default)：路径缺失时给默认值（注意对象必须存在）
    const int         missing = parsed["sensor"].value("gain", -1);
    std::printf("3) name=%s rate=%d first_axis=%.1f gain(默认)=%d\n", name.c_str(), rate, first,
                missing);

    // =========================================================================
    //  4) 类型判断与容器遍历
    // =========================================================================
    std::printf("4) is_object=%d is_array=%d size=%zu\n", parsed["sensor"].is_object(),
                parsed["filters"].is_array(), parsed["filters"].size());

    for (const auto& f : parsed["filters"]) {
        std::printf("   filter: %s\n", f.get<std::string>().c_str());
    }
    for (auto it = parsed["sensor"].begin(); it != parsed["sensor"].end(); ++it) {
        std::printf("   sensor.%-8s -> %s\n", it.key().c_str(), it.value().dump().c_str());
    }

    // =========================================================================
    //  5) 自定义类型映射
    // =========================================================================
    Task task;
    task.name     = "patrol";
    task.priority = 5;
    task.path     = {{0, 0}, {1, 2}, {3, 4}};

    json task_json = task;                  // 结构体 -> JSON（NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE）
    Task back      = task_json.get<Task>(); // JSON -> 结构体
    std::printf("5) 结构体往返: %s, 路径点数=%zu\n", back.name.c_str(), back.path.size());

    // =========================================================================
    //  6) 序列化选项
    // =========================================================================
    std::printf("6a) 紧凑: %s\n", task_json.dump().c_str());
    std::printf("6b) 缩进2:\n%s\n", task_json.dump(2).c_str());
    std::printf("6c) 中文不转义: %s\n",
                json({{"greeting", "你好"}}).dump(-1, ' ', false).c_str());
    std::printf("6d) 数组按行排布: %s\n",
                json::array({1, 2, 3}).dump(-1, ' ', /*ensure_ascii=*/true).c_str());

    // =========================================================================
    //  7) 文件读写
    // =========================================================================
    {
        std::ofstream out("output/native_json.json");
        out << task_json.dump(2);
    }
    {
        std::ifstream in("output/native_json.json");
        json reloaded = json::parse(in);
        std::printf("7) 文件往返成功: priority=%d\n", reloaded["priority"].get<int>());
    }

    // =========================================================================
    //  8) 合并 / 删除 / 判存在
    // =========================================================================
    json a = {{"keep", 1}, {"override", "old"}};
    json b = {{"override", "new"}, {"added", true}};
    a.update(b);  // 浅合并
    a.erase("keep");
    std::printf("8) 合并后: %s, contains(added)=%d\n", a.dump().c_str(), a.contains("added"));

    return 0;
}
