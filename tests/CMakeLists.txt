# =============================================================================
#  单元测试
#
#  交叉编译时仍然构建测试二进制（用来验证目标代码能过编译和链接），
#  但不注册到 CTest —— 目标板上没法用 ctest 直接跑。
#
#  注册方式：**逐用例注册**。
#  用例名在 configure 期直接从源码里扫出来（MYTEST_CASE(<name>)），
#  所以不需要先构建出可执行文件、交叉编译也能注册，且没有"配置晚一轮"的坑。
#
#       ctest -R preprocess          # 只跑 preprocess 那批
#       ctest -j8                    # 逐用例并行
#       ctest -R preprocess_nms -V   # 单个用例，看完整输出
#
#  另外保留一个 --list 自检：用例注册机制坏了（某个 TU 忘了 MYTEST_SUITE）会暴露。
# =============================================================================
set(_test_sources test_main.cpp test_framework_self.cpp test_core.cpp test_hash.cpp)
set(_test_suites core hash)
set(_test_links mylib_core)

if(TARGET mylib_log)
    list(APPEND _test_sources test_log.cpp)
    list(APPEND _test_suites log)
    list(APPEND _test_links mylib_log)
endif()
if(TARGET mylib_json)
    list(APPEND _test_sources test_json.cpp test_config.cpp)
    list(APPEND _test_suites json config)
    list(APPEND _test_links mylib_json)
endif()

# 0.1.2 的三个底座组件
if(TARGET mylib_io)
    list(APPEND _test_sources test_io.cpp)
    list(APPEND _test_suites io)
    list(APPEND _test_links mylib_io)
endif()
if(TARGET mylib_time)
    list(APPEND _test_sources test_time.cpp)
    list(APPEND _test_suites time)
    list(APPEND _test_links mylib_time)
endif()
if(TARGET mylib_thread)
    list(APPEND _test_sources test_thread.cpp)
    list(APPEND _test_suites thread)
    list(APPEND _test_links mylib_thread)
endif()
# serial 的自测分两层：参数校验/枚举在哪个平台都能跑；真正的收发放到 PTY 上，
# 只在 POSIX 上跑（Windows 没有无名虚拟串口，那部分用例会 MYTEST_SKIP）。
if(TARGET mylib_serial)
    list(APPEND _test_sources test_serial.cpp)
    list(APPEND _test_suites serial)
    list(APPEND _test_links mylib_serial)
endif()
if(TARGET mylib_math)
    list(APPEND _test_sources test_math.cpp)
    list(APPEND _test_suites math)
    list(APPEND _test_links mylib_math)
endif()
if(TARGET mylib_infer)
    list(APPEND _test_sources test_infer.cpp)
    list(APPEND _test_suites infer)
    list(APPEND _test_links mylib_infer)
    # ORT 模型端口测试要读源码树 models/；ctest 工作目录是 build/
    list(APPEND _test_defs "MYLIB_TEST_MODELS_DIR=\"${PROJECT_SOURCE_DIR}/models\"")
endif()

# preprocess 是独立 target（image/http/cv 也会用到），不能挂在 infer 下面：
# 否则 -DMYLIB_ENABLE_INFER=OFF 时这 25 个用例会静默消失。
if(TARGET mylib_preprocess)
    list(APPEND _test_sources test_preprocess.cpp)
    list(APPEND _test_suites preprocess)
    list(APPEND _test_links mylib_preprocess)
endif()

# 审计补齐的测试空洞：跨组件（time/json/math/io/net/thread/image），
# 所以放在最后，按"这些 target 都存在"来门控（math/json 也要：最小配置会把它们关掉）。
if(TARGET mylib_image AND TARGET mylib_net AND TARGET mylib_thread AND TARGET mylib_math AND TARGET mylib_json)
    list(APPEND _test_sources test_gaps.cpp)
    list(APPEND _test_suites gaps)
    list(APPEND _test_links mylib_image mylib_net mylib_thread mylib_io mylib_json mylib_math mylib_time)
endif()

if(TARGET mylib_dsp)
    list(APPEND _test_sources test_dsp.cpp test_dsp_stream.cpp)
    list(APPEND _test_suites dsp dsp_stream)
    list(APPEND _test_links mylib_dsp)
endif()

if(TARGET mylib_ml)
    list(APPEND _test_sources test_ml.cpp)
    list(APPEND _test_suites ml)
    list(APPEND _test_links mylib_ml)
    # 模型路径宏已由 mylib_infer 分支统一挂上（ml 依赖 infer，正常两者都开）
endif()

if(TARGET mylib_net)
    list(APPEND _test_sources test_net.cpp)
    list(APPEND _test_suites net)
    list(APPEND _test_links mylib_net)
endif()

if(TARGET mylib_http)
    list(APPEND _test_sources test_http.cpp)
    list(APPEND _test_suites http)
    list(APPEND _test_links mylib_http)
endif()

if(TARGET mylib_image)
    list(APPEND _test_sources test_image.cpp)
    list(APPEND _test_suites image)
    list(APPEND _test_links mylib_image)
endif()

if(TARGET mylib_cv)
    list(APPEND _test_sources test_cv.cpp)
    list(APPEND _test_suites cv)
    list(APPEND _test_links mylib_cv)
endif()

add_executable(mylib_tests ${_test_sources})
target_link_libraries(mylib_tests PRIVATE ${_test_links} mylib_warnings mylib_sanitize)
if(_test_defs)
    target_compile_definitions(mylib_tests PRIVATE ${_test_defs})
endif()
set_target_properties(mylib_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${cfg}" cfg_upper)
    set_target_properties(mylib_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg_upper} "${CMAKE_BINARY_DIR}/bin/${cfg}")
endforeach()

# -----------------------------------------------------------------------------
#  逐用例注册
# -----------------------------------------------------------------------------
# 扫源码拿用例名：MYTEST_CASE(name) 里的 name 是字面量标识符，正则足够可靠。
# 只扫参与编译的文件，所以"组件关掉后用例自动消失"这件事和构建保持一致。
set(_case_names "")
foreach(_src ${_test_sources})
    if(_src STREQUAL "test_main.cpp")
        continue()
    endif()
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${_src}" _text)
    string(REGEX MATCHALL "MYTEST_CASE[ \t\r\n]*\\([ \t\r\n]*[A-Za-z0-9_]+[ \t\r\n]*\\)" _hits "${_text}")
    foreach(_hit ${_hits})
        string(REGEX REPLACE ".*\\([ \t\r\n]*([A-Za-z0-9_]+)[ \t\r\n]*\\).*" "\\1" _name "${_hit}")
        list(APPEND _case_names "${_name}")
    endforeach()
endforeach()
list(REMOVE_DUPLICATES _case_names)
list(LENGTH _case_names _case_count)

if(_case_count EQUAL 0)
    message(FATAL_ERROR "用例扫描结果为 0 —— MYTEST_CASE 正则或源码结构出问题了，拒绝生成一个空测试集")
endif()

# 这个构建里如果有**真的**推理后端，ml 用例就不许 skip（skip 意味着 golden 对拍没跑）。
# --fail-on-skip 的用意是"本该跑却静默跳过"（典型：有 ORT 时 ml 的对拍没跑）。
# 但它不适用于**环境决定**的跳过：serial 的 9 条 PTY 用例在 Windows 上必然跳过
# （没装 com0com 之类虚拟串口驱动）；ncnn 后端在 CI 的 windows-msvc job 里是关的。
# 这两类按平台事实放行，ORT 的模型端口用例仍然严格。
if(MYLIB_ENABLE_ML AND MYLIB_INFER_BACKEND STREQUAL "onnxruntime")
    set(_strict_skip TRUE)
endif()

if(NOT CMAKE_CROSSCOMPILING)
    foreach(_name ${_case_names})
        set(_this_args "")
        if(_strict_skip AND NOT _name MATCHES "^(serial_|infer_ncnn_)")
            list(APPEND _this_args --fail-on-skip)
        endif()
        add_test(NAME mylib_case_${_name} COMMAND mylib_tests --case=${_name}
                 ${_this_args})
        set_tests_properties(mylib_case_${_name} PROPERTIES
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            LABELS "${_name}"
            TIMEOUT 300)
    endforeach()
    # 自检：用例注册机制坏了（某个 TU 忘了 MYTEST_SUITE）这里会暴露
    add_test(NAME mylib_list_cases COMMAND mylib_tests --list)
    set_tests_properties(mylib_list_cases PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        PASS_REGULAR_EXPRESSION "core:"
        TIMEOUT 60)

    # 自检 2：二进制里实际注册的用例数必须等于源码扫描数。
    # 少了（新增 test_*.cpp 但没加进 _test_sources）或多了（扫描漏了某个文件）
    # 都会在这里变成失败，而不是"少跑几个用例还全绿"。
    add_test(NAME mylib_case_count COMMAND mylib_tests --expect-cases=${_case_count})
    set_tests_properties(mylib_case_count PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        TIMEOUT 60)

    message(STATUS "单元测试：${_case_count} 个用例已按用例注册到 CTest")
else()
    message(STATUS "交叉编译：跳过 CTest 注册（目标二进制只能拷到板子上运行）")
endif()
