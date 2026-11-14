# =============================================================================
#  单元测试
#
#  交叉编译时仍然构建测试二进制（用来验证目标代码能过编译和链接），
#  但不注册到 CTest —— 目标板上没法用 ctest 直接跑。
# =============================================================================
set(_test_sources
    test_main.cpp
    test_core.cpp)

set(_test_links mylib_core)

if(TARGET mylib_log)
    list(APPEND _test_sources test_log.cpp)
    list(APPEND _test_links mylib_log)
endif()
if(TARGET mylib_json)
    list(APPEND _test_sources test_json.cpp)
    list(APPEND _test_links mylib_json)
endif()
if(TARGET mylib_math)
    list(APPEND _test_sources test_math.cpp)
    list(APPEND _test_links mylib_math)
endif()
if(TARGET mylib_infer)
    list(APPEND _test_sources test_infer.cpp test_preprocess.cpp)
    list(APPEND _test_links mylib_infer)
endif()

if(TARGET mylib_cv)
    list(APPEND _test_sources test_cv.cpp)
    list(APPEND _test_links mylib_cv)
endif()

add_executable(mylib_tests ${_test_sources})
target_link_libraries(mylib_tests PRIVATE ${_test_links} mylib_warnings)
set_target_properties(mylib_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${cfg}" cfg_upper)
    set_target_properties(mylib_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg_upper} "${CMAKE_BINARY_DIR}/bin/${cfg}")
endforeach()

if(NOT CMAKE_CROSSCOMPILING)
    add_test(NAME mylib_unit_tests COMMAND mylib_tests)
    set_tests_properties(mylib_unit_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        TIMEOUT 120)
else()
    message(STATUS "交叉编译：跳过 CTest 注册（目标二进制只能拷到板子上运行）")
endif()
