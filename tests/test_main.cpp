#include "test_framework.hpp"

#include <cstdio>
#include <cstring>

// 支持三种过滤方式，ctest 会据此按用例逐个注册：
//   mylib_tests --case=preprocess_nms       只跑这一个用例（名字必须完全一致）
//   mylib_tests --suite=preprocess          跑一个套件
//   mylib_tests --filter=nms                按用例名子串
//   mylib_tests --list                      列出所有用例（suite:name）
int main(int argc, char** argv) {
    const char* suite_filter = nullptr;
    const char* name_filter  = nullptr;
    bool        exact_name   = false;
    bool        list         = false;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strncmp(a, "--suite=", 8) == 0) {
            suite_filter = a + 8;
        } else if (std::strncmp(a, "--case=", 7) == 0) {
            name_filter = a + 7;
            exact_name  = true;
        } else if (std::strncmp(a, "--filter=", 9) == 0) {
            name_filter = a + 9;
            exact_name  = false;
        } else if (std::strcmp(a, "--list") == 0) {
            list = true;
        } else if (a[0] != '-') {
            name_filter = a;  // 位置参数当作用例名子串
            exact_name  = false;
        } else {
            std::printf("未知参数: %s\n"
                        "用法: %s [--case=NAME] [--suite=NAME] [--filter=SUBSTR] [--list]\n",
                        a, argv[0]);
            return 2;
        }
    }

    if (list) {
        mytest::list_cases();
        return 0;
    }
    return mytest::run(name_filter, suite_filter, exact_name);
}
