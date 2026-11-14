# =============================================================================
#  命令行工具
#
#  这些工具不随库一起安装，只有在本工程作为顶层工程构建时才编译。
# =============================================================================
if(NOT MYLIB_IS_TOP_LEVEL)
    return()
endif()

set(MYLIB_TOOL_BIN_DIR "${CMAKE_BINARY_DIR}/bin")

function(mylib_add_tool name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LINK" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE ${ARG_LINK} mylib_warnings)
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${MYLIB_TOOL_BIN_DIR}"
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)
    foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
        string(TOUPPER "${cfg}" cfg_upper)
        set_target_properties(${name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${cfg_upper} "${MYLIB_TOOL_BIN_DIR}/${cfg}")
    endforeach()
    mylib_copy_runtime_dlls(${name})
endfunction()

if(TARGET mylib_infer)
    mylib_add_tool(model_inspect SOURCES model_inspect/main.cpp LINK mylib_infer)
endif()
