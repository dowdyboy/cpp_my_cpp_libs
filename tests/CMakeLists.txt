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
set(_test_sources test_main.cpp test_core.cpp)
set(_test_suites core)
set(_test_links mylib_core)

if(TARGET mylib_log)
    list(APPEND _test_sources test_log.cpp)
    list(APPEND _test_suites log)
    list(APPEND _test_links mylib_log)
endif()
if(TARGET mylib_json)
    list(APPEND _test_sources test_json.cpp)
    list(APPEND _test_suites json)
    list(APPEND _test_links mylib_json)
endif()
if(TARGET mylib_math)
    list(APPEND _test_sources test_math.cpp)
    list(APPEND _test_suites math)
    list(APPEND _test_links mylib_math)
endif()
if(TARGET mylib_infer)
    list(APPEND _test_sources test_infer.cpp test_preprocess.cpp)
    list(APPEND _test_suites infer preprocess)
    list(APPEND _test_links mylib_infer)
endif()

if(TARGET mylib_cv)
    list(APPEND _test_sources test_cv.cpp)
    list(APPEND _test_suites cv)
    list(APPEND _test_links mylib_cv)
endif()

add_executable(mylib_tests ${_test_sources})
target_link_libraries(mylib_tests PRIVATE ${_test_links} mylib_warnings mylib_sanitize)
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

if(NOT CMAKE_CROSSCOMPILING)
    foreach(_name ${_case_names})
        add_test(NAME mylib_case_${_name} COMMAND mylib_tests --case=${_name})
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

    message(STATUS "单元测试：${_case_count} 个用例已按用例注册到 CTest")
else()
    message(STATUS "交叉编译：跳过 CTest 注册（目标二进制只能拷到板子上运行）")
endif()
