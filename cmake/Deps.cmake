# =============================================================================
#  三方依赖挂载层
#
#  目标：
#    - 完全不依赖网络。resources/*.zip 是唯一真源。
#    - 首次 configure 时自动解压到 third_party/（可关闭）。
#    - 统一暴露为 `deps::*` 目标，换库只改这一个文件。
# =============================================================================

set(MYLIB_DEPS_DIR "${MYLIB_DEPS_DIR}" CACHE PATH "" FORCE)

# ---------------------------------------------------------------- 解压辅助
# mylib_extract_zip(<zip> <dest> [PATTERNS ...])
function(mylib_extract_zip zip_path dest)
    cmake_parse_arguments(ARG "" "" "PATTERNS" ${ARGN})
    if(NOT EXISTS "${zip_path}")
        message(WARNING "[deps] 找不到压缩包: ${zip_path}")
        return()
    endif()
    message(STATUS "[deps] 解压 ${zip_path}")
    if(ARG_PATTERNS)
        file(ARCHIVE_EXTRACT INPUT "${zip_path}" DESTINATION "${dest}" PATTERNS ${ARG_PATTERNS})
    else()
        file(ARCHIVE_EXTRACT INPUT "${zip_path}" DESTINATION "${dest}")
    endif()
endfunction()

# mylib_require_dep(<dir-name> <zip-glob> [DESTINATION <dir>] [PATTERNS ...])
#   返回变量 MYLIB_DEP_<dir-name> = 解压后的绝对目录
#   DESTINATION: 压缩包内没有顶层目录时（如 json-include.zip），
#                需要手动指定解压目标目录
function(mylib_require_dep dep_dir zip_glob)
    cmake_parse_arguments(ARG "" "DESTINATION" "PATTERNS" ${ARGN})
    set(dep_path "${MYLIB_DEPS_DIR}/${dep_dir}")
    if(NOT EXISTS "${dep_path}")
        if(MYLIB_AUTO_EXTRACT_DEPS)
            file(GLOB _zips "${MYLIB_RESOURCES_DIR}/${zip_glob}")
            if(_zips)
                list(GET _zips 0 _zip)
                if(ARG_DESTINATION)
                    set(_dest "${ARG_DESTINATION}")
                else()
                    set(_dest "${MYLIB_DEPS_DIR}")
                endif()
                mylib_extract_zip("${_zip}" "${_dest}" PATTERNS ${ARG_PATTERNS})
            else()
                message(WARNING "[deps] ${dep_dir} 不存在，且 resources 中没有匹配 ${zip_glob} 的压缩包")
            endif()
        endif()
    endif()
    set(MYLIB_DEP_${dep_dir} "${dep_path}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------- ELF 架构探测
# 找 ELF 魔数再解析 e_machine（偏移 18，小端 2 字节）。
#   0x28 -> arm, 0xB7 -> aarch64, 0x3E -> x86_64, 0x03 -> x86
#
# 为什么不能只看文件开头：静态库 .a 是 GNU ar 归档，前面是符号表
# （ncnn 的符号表就有 100KB+），真正的 ELF 成员在更后面。
# 所以分块扫描，最多扫 2MB —— 足够覆盖常见静态库。
function(_mylib_elf_arch file_path out_var)
    set(_arch "unknown")
    if(NOT EXISTS "${file_path}")
        set(${out_var} "unknown" PARENT_SCOPE)
        return()
    endif()

    set(_limit 2097152)
    set(_chunk 131072)
    set(_off 0)
    set(_pos -1)

    while(_off LESS _limit)
        file(READ "${file_path}" _hex OFFSET ${_off} LIMIT ${_chunk} HEX)
        if(_hex STREQUAL "")
            break()
        endif()
        string(FIND "${_hex}" "7f454c46" _p)
        if(NOT _p EQUAL -1)
            math(EXPR _pos "${_off} + ${_p} / 2")
            break()
        endif()
        math(EXPR _off "${_off} + ${_chunk}")
    endwhile()

    if(_pos GREATER -1)
        file(READ "${file_path}" _ehdr OFFSET ${_pos} LIMIT 20 HEX)
        string(LENGTH "${_ehdr}" _len)
        if(_len GREATER_EQUAL 40)
            string(SUBSTRING "${_ehdr}" 36 4 _machine)
            if(_machine STREQUAL "2800")
                set(_arch "arm")
            elseif(_machine STREQUAL "b700")
                set(_arch "aarch64")
            elseif(_machine STREQUAL "3e00")
                set(_arch "x86_64")
            elseif(_machine STREQUAL "0300")
                set(_arch "x86")
            endif()
        endif()
    endif()

    set(${out_var} "${_arch}" PARENT_SCOPE)
endfunction()

# 判断某个目录是不是"Linux/交叉编译的 OpenCV 安装树"（里面是 .so，不是 .lib）。
# 目录可能是安装根、也可能是根下面 3 层的 lib/cmake/opencv4，所以往上找几级。
function(_mylib_is_elf_opencv d out_var)
    set(_hit 0)
    foreach(_up "" "/.." "/../.." "/../../..")
        if(EXISTS "${d}${_up}/lib/libopencv_core.so")
            set(_hit 1)
            break()
        endif()
    endforeach()
    set(${out_var} ${_hit} PARENT_SCOPE)
endfunction()

# 目标架构（与 _mylib_elf_arch 的返回值同一套记号），供各依赖做架构校验。
# 交叉编译时这一条是刚需：把 x64 的 .so/.lib 链进 ARM 产物，报错会非常难懂。
if(MYLIB_ARCH STREQUAL "arm")
    set(MYLIB_TARGET_ARCH "arm")
elseif(MYLIB_ARCH STREQUAL "aarch64")
    set(MYLIB_TARGET_ARCH "aarch64")
elseif(MYLIB_ARCH STREQUAL "x86_64")
    set(MYLIB_TARGET_ARCH "x86_64")
elseif(MYLIB_ARCH STREQUAL "x86")
    set(MYLIB_TARGET_ARCH "x86")
else()
    set(MYLIB_TARGET_ARCH "")
endif()

# ---------------------------------------------------------------- Eigen
if(MYLIB_ENABLE_MATH)
    mylib_require_dep(eigen-5.0.0 "eigen-*.zip")
    if(EXISTS "${MYLIB_DEP_eigen-5.0.0}/Eigen/Core")
        # 只取头文件，不跑 Eigen 自己的 CMakeLists（省掉 2000+ 文件的项目配置，
        # 也避免它给消费方塞全局编译选项）。
        add_library(deps_eigen INTERFACE)
        add_library(deps::eigen ALIAS deps_eigen)
        target_include_directories(deps_eigen INTERFACE "${MYLIB_DEP_eigen-5.0.0}")
        # 不覆盖 Eigen 的默认对齐设置：C++17 的 aligned new 已能保证
        # std::vector<Eigen::Vector4d> 之类类型的安全分配（见 docs/02-API设计.md）
        message(STATUS "[deps] Eigen: ${MYLIB_DEP_eigen-5.0.0}")
    else()
        message(FATAL_ERROR "[deps] Eigen 头文件缺失，请检查 resources/eigen-*.zip")
    endif()
endif()

# ---------------------------------------------------------------- spdlog
if(MYLIB_ENABLE_LOG)
    mylib_require_dep(spdlog-1.17.0 "spdlog-*.zip")
    if(EXISTS "${MYLIB_DEP_spdlog-1.17.0}/CMakeLists.txt")
        # 嵌入到宿主工程时，宿主可能已经自己带了一份 spdlog。
        # 那种情况下直接复用，再 add_subdirectory 一次会因为目标重名直接失败。
        if(TARGET spdlog)
            message(STATUS "[deps] spdlog: 复用宿主工程已有的 target（不再解压/编译）")
        else()
            set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
            set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "" FORCE)
            set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
            set(SPDLOG_BUILD_TESTS_HO OFF CACHE BOOL "" FORCE)
            set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
            set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
            set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
            set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
            set(SPDLOG_USE_STD_FORMAT OFF CACHE BOOL "" FORCE)
            add_subdirectory("${MYLIB_DEP_spdlog-1.17.0}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/spdlog" EXCLUDE_FROM_ALL)
            message(STATUS "[deps] spdlog: ${MYLIB_DEP_spdlog-1.17.0}")
        endif()
        # spdlog::spdlog 本身是 ALIAS，不能再给它起别名，套一层 INTERFACE
        add_library(deps_spdlog INTERFACE)
        target_link_libraries(deps_spdlog INTERFACE spdlog::spdlog)
        add_library(deps::spdlog ALIAS deps_spdlog)
    else()
        message(FATAL_ERROR "[deps] spdlog 源码缺失，请检查 resources/spdlog-*.zip")
    endif()
endif()

# ---------------------------------------------------------------- nlohmann/json
if(MYLIB_ENABLE_JSON)
    # json-include.zip 里没有顶层目录（直接是 include/nlohmann/...），所以要指定 DESTINATION
    mylib_require_dep(nlohmann-json-3.12.0 "json-include*.zip"
        DESTINATION "${MYLIB_DEPS_DIR}/nlohmann-json-3.12.0")
    if(EXISTS "${MYLIB_DEP_nlohmann-json-3.12.0}/include/nlohmann/json.hpp")
        add_library(deps_json INTERFACE)
        add_library(deps::json ALIAS deps_json)
        target_include_directories(deps_json INTERFACE "${MYLIB_DEP_nlohmann-json-3.12.0}/include")
        message(STATUS "[deps] nlohmann/json: ${MYLIB_DEP_nlohmann-json-3.12.0}")
    else()
        message(FATAL_ERROR "[deps] nlohmann/json 头文件缺失，请检查 resources/json-include*.zip")
    endif()
endif()

# ---------------------------------------------------------------- pocketfft（信号处理）
# 单头文件 FFT（BSD-3，NumPy/SciPy 同源）。没有 CMake、没有 .c，
# 所以和 stb 一样：解压出目录，挂成 INTERFACE 目标。
#
# 为什么不用 FFTW：GPL；不用 kissfft：精度和"任意长度"支持不如 pocketfft；
# 不用 CMSIS-DSP：偏 Cortex-M，NEON 加速对 A9 收益有限（见 docs/08）。
if(MYLIB_ENABLE_DSP)
    mylib_require_dep(pocketfft-4d865cb "pocketfft-*.zip"
        DESTINATION "${MYLIB_DEPS_DIR}/pocketfft-4d865cb")
    if(EXISTS "${MYLIB_DEP_pocketfft-4d865cb}/pocketfft_hdronly.h")
        add_library(deps_pocketfft INTERFACE)
        add_library(deps::pocketfft ALIAS deps_pocketfft)
        target_include_directories(deps_pocketfft INTERFACE "${MYLIB_DEP_pocketfft-4d865cb}")
        # pocketfft 默认用异常报错。本库的约定是"公共 API 不抛异常"，
        # 所以统一让它走 abort（和 Result::value() 违反前置条件时的策略一致）；
        # 输入合法性由 my::dsp 层先校验，正常路径不会碰到这个分支。
        # 顺带一个好处：DSP 组件也能在 -fno-exceptions 下编。
        target_compile_definitions(deps_pocketfft INTERFACE
            POCKETFFT_NO_MULTITHREADING
            POCKETFFT_NO_EXCEPTIONS)
        message(STATUS "[deps] pocketfft: ${MYLIB_DEP_pocketfft-4d865cb}")
    else()
        message(FATAL_ERROR
            "[deps] pocketfft 头文件缺失。请检查 resources/pocketfft-*.zip，"
            "或跑一次 scripts/fetch-deps.ps1（Linux: scripts/fetch-deps.sh）")
    endif()
endif()

# ---------------------------------------------------------------- cpp-httplib（HTTP）
# 单头文件 HTTP 客户端 + 服务端（MIT）。项目里只会在 src/http/http.cpp 这一个 TU
# 里 include 它，公共头不暴露（和 stb / pocketfft 同一个套路）。
if(MYLIB_ENABLE_HTTP)
    mylib_require_dep(httplib-0.66.0 "httplib-*.zip"
        DESTINATION "${MYLIB_DEPS_DIR}/httplib-0.66.0")
    if(EXISTS "${MYLIB_DEP_httplib-0.66.0}/httplib.h")
        add_library(deps_httplib INTERFACE)
        add_library(deps::httplib ALIAS deps_httplib)
        target_include_directories(deps_httplib INTERFACE "${MYLIB_DEP_httplib-0.66.0}")
        if(WIN32)
            # httplib 要求 Win10+（否则直接 #error），并依赖 winsock
            target_compile_definitions(deps_httplib INTERFACE _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
            target_link_libraries(deps_httplib INTERFACE ws2_32)
        endif()
        # 服务端线程池：板端（2 核）要设成 2；默认给桌面用的 4
        if(NOT DEFINED MYLIB_HTTP_THREAD_POOL)
            set(MYLIB_HTTP_THREAD_POOL "4" CACHE STRING "HTTP 服务端线程池大小（板端建议 2）")
        endif()
        target_compile_definitions(deps_httplib INTERFACE
            CPPHTTPLIB_THREAD_POOL_COUNT=${MYLIB_HTTP_THREAD_POOL})
        message(STATUS "[deps] cpp-httplib: ${MYLIB_DEP_httplib-0.66.0}（线程池 ${MYLIB_HTTP_THREAD_POOL}）")
    else()
        message(FATAL_ERROR
            "[deps] httplib.h 缺失。请检查 resources/httplib-*.zip，"
            "或跑一次 scripts/fetch-deps.ps1（Linux: scripts/fetch-deps.sh）")
    endif()
endif()

# ---------------------------------------------------------------- stb_image
# 图像组件的解码/编码底座。**只有两个 .h**（stb 是单头文件库），
# 所以不走 add_subdirectory —— 没有 CMake、没有 .c、也没什么可配置的。
#
# 为什么不用 OpenCV 代替：板端（Zynq armv7）没有 OpenCV，而相机链路
# 只缺"解码 + resize"这两件事，为了它们把整个 OpenCV 交叉编译上板不值得。
# 选型与实测见 docs/07-图像链路与无异常构建.md。
if(MYLIB_ENABLE_IMAGE)
    # 压缩包里没有顶层目录（直接是 stb_image.h），所以显式指定 DESTINATION
    mylib_require_dep(stb-image-2.32 "stb-image-*.zip"
        DESTINATION "${MYLIB_DEPS_DIR}/stb-image-2.32")
    if(EXISTS "${MYLIB_DEP_stb-image-2.32}/stb_image.h")
        add_library(deps_stb INTERFACE)
        add_library(deps::stb ALIAS deps_stb)
        target_include_directories(deps_stb INTERFACE "${MYLIB_DEP_stb-image-2.32}")
        message(STATUS "[deps] stb_image: ${MYLIB_DEP_stb-image-2.32}")
    else()
        message(FATAL_ERROR
            "[deps] stb_image 头文件缺失。请检查 resources/stb-image-*.zip，"
            "或跑一次 scripts/fetch-deps.ps1（Linux: scripts/fetch-deps.sh）")
    endif()
endif()

# ---------------------------------------------------------------- ONNX Runtime
# ---------------------------------------------------------------- ONNX Runtime
# 判定顺序：
#   1) 用户显式给出 MYLIB_ONNXRUNTIME_ROOT
#   2) 扫描 third_party/onnxruntime-*
#   3) 都没有就从 resources 解压
#   4) 仍不满足 -> 回退 stub 后端（infer 组件照样能编译，只是不能加载模型）
#
# 关键点：third_party 下可能同时有 Windows 包和 Linux 包，甚至不同架构的 Linux 包。
# 只按目录名猜不够可靠，这里直接读 ELF 头里的 e_machine 字段确认架构，
# 避免把 ARM 的 .so 链进 x86 程序（那种报错非常难懂）。
if(MYLIB_ENABLE_INFER AND MYLIB_ENABLE_ONNXRUNTIME)
    set(MYLIB_INFER_BACKEND "none")

    # ---- 目标架构 ----
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|armv7|armv7l|armhf)$")
        set(_ort_want_arch "arm")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        set(_ort_want_arch "aarch64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        set(_ort_want_arch "x86_64")
    else()
        set(_ort_want_arch "")
    endif()

    # ---- 读 ELF 头判断 .so 架构（见文件顶部的 _mylib_elf_arch）----
    # e_machine 是偏移 18 处的 2 字节小端整数：ARM=0x28, AArch64=0xB7, x86-64=0x3E

    # ---- 收集候选目录 ----
    # 顺序 = 用户显式指定（最高优先） + third_party 下的自动扫描（file(GLOB) 有序，可复现）
    set(_ort_candidates "")
    if(MYLIB_ONNXRUNTIME_ROOT)
        if(EXISTS "${MYLIB_ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
            list(APPEND _ort_candidates "${MYLIB_ONNXRUNTIME_ROOT}")
        else()
            # 旧的构建目录里可能残留一个早就被删掉/换掉的路径，不清理就会一直选不上新包
            message(STATUS "[deps] 忽略失效的 MYLIB_ONNXRUNTIME_ROOT=${MYLIB_ONNXRUNTIME_ROOT}"
                           "（目录里没有 onnxruntime_cxx_api.h）")
            set(MYLIB_ONNXRUNTIME_ROOT "" CACHE PATH "ONNX Runtime 根目录（留空则自动探测）" FORCE)
        endif()
    endif()
    file(GLOB _ort_dirs "${MYLIB_DEPS_DIR}/onnxruntime-*")
    list(APPEND _ort_candidates ${_ort_dirs})

    # ---- 一个都还没解压 -> 从 resources 解压 ----
    if(NOT _ort_candidates AND MYLIB_AUTO_EXTRACT_DEPS)
        file(GLOB _ort_zips "${MYLIB_RESOURCES_DIR}/onnxruntime-*.zip")
        if(_ort_zips)
            list(GET _ort_zips 0 _ort_zip)
            # 不能用 NAME_WE：它会把 "1.23.2" 里的第一个点当成扩展名分隔符
            get_filename_component(_ort_pkg_name "${_ort_zip}" NAME)
            string(REGEX REPLACE "\\.zip$" "" _ort_pkg_name "${_ort_pkg_name}")

            mylib_extract_zip("${_ort_zip}" "${MYLIB_DEPS_DIR}")

            # 调试符号动辄几百 MB，装完即删
            file(GLOB_RECURSE _ort_pdbs "${MYLIB_DEPS_DIR}/${_ort_pkg_name}/lib/*.pdb")
            if(_ort_pdbs)
                file(REMOVE ${_ort_pdbs})
                message(STATUS "[deps] 已删除 ONNX Runtime 的 .pdb 调试符号")
            endif()

            file(GLOB _ort_dirs "${MYLIB_DEPS_DIR}/onnxruntime-*")
            list(APPEND _ort_candidates ${_ort_dirs})
        endif()
    endif()

    # ---- 挑出第一个架构匹配的 ----
    set(_ort_root "")
    set(_ort_rejected "")
    foreach(_c ${_ort_candidates})
        if(NOT EXISTS "${_c}/include/onnxruntime_cxx_api.h")
            continue()
        endif()
        if(WIN32)
            if(MSVC AND EXISTS "${_c}/lib/onnxruntime.lib")
                set(_ort_root "${_c}")
                break()
            endif()
            if(EXISTS "${_c}/lib/onnxruntime.lib")
                list(APPEND _ort_rejected "${_c}（MSVC 导入库，MinGW 不可用）")
            else()
                list(APPEND _ort_rejected "${_c}（不是 Windows x64 包）")
            endif()
        else()
            if(NOT _ort_want_arch)
                continue()
            endif()
            if(EXISTS "${_c}/lib/libonnxruntime.so")
                _mylib_elf_arch("${_c}/lib/libonnxruntime.so" _so_arch)
                if(_so_arch STREQUAL _ort_want_arch)
                    set(_ort_root "${_c}")
                    break()
                endif()
                list(APPEND _ort_rejected "${_c} (${_so_arch})")
            endif()
        endif()
    endforeach()

    if(_ort_root)
        set(MYLIB_INFER_BACKEND "onnxruntime")
        # 只记录"探测结果"，**不回写 CACHE**。
        # 回写（set(... CACHE ... FORCE)）会让下一次 configure 把上次的选择当成人
        # 用户意图放在候选第一位，于是 third_party 里新出现的包永远选不上 —— 同一份源码、
        # 同一个 preset，新旧构建目录会链上不同版本的 ORT（1.23.2 vs 1.18.1）。
        set(MYLIB_ORT_ROOT_DETECTED "${_ort_root}")
        add_library(deps_ort INTERFACE)
        add_library(deps::onnxruntime ALIAS deps_ort)
        target_include_directories(deps_ort INTERFACE "${_ort_root}/include")
        target_compile_definitions(deps_ort INTERFACE MYLIB_HAVE_ONNXRUNTIME=1)
        if(WIN32)
            target_link_libraries(deps_ort INTERFACE "${_ort_root}/lib/onnxruntime.lib")
        else()
            target_link_libraries(deps_ort INTERFACE "${_ort_root}/lib/libonnxruntime.so")
        endif()
        get_filename_component(_ort_name "${_ort_root}" NAME)
        message(STATUS "[deps] ONNX Runtime: ${_ort_name} (${_ort_want_arch})")
    elseif(_ort_rejected)
        string(REPLACE ";" "\n    " _rej "${_ort_rejected}")
        if(WIN32 AND NOT MSVC)
            message(STATUS "[deps] 找到了 ONNX Runtime 包，但当前是 MinGW 构建，**故意不用**：\n"
                           "      ONNX Runtime 的 Windows 包是 MSVC ABI（.lib + .dll），\n"
                           "      跨 CRT 传 std::string 是未定义行为，链上了也可能诡异崩溃。\n"
                           "      -> infer 回退 stub 后端；要真跑推理请用 win-msvc 预设。\n"
                           "      候选: ${_rej}")
        else()
            message(STATUS "[deps] 找到了 ONNX Runtime 包但架构不匹配：\n    ${_rej}\n"
                           "      目标: ${CMAKE_SYSTEM_PROCESSOR} —— 回退 stub 后端")
        endif()
    else()
        message(STATUS "[deps] 未找到 ONNX Runtime —— infer 组件使用 stub 后端")
    endif()
endif()
# ---------------------------------------------------------------- ncnn（备选推理后端）
# 定位：ONNX Runtime 的备份路线。模型要先转成 .param + .bin。
# 默认不启用（MYLIB_ENABLE_NCNN=OFF）；启用后如果找不到库就只警告，不阻断构建。
if(MYLIB_ENABLE_NCNN)
    set(MYLIB_HAVE_NCNN OFF)

    # 目标架构（和 ONNX Runtime 用同一套判定）
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|armv7|armv7l|armhf)$")
        set(_ncnn_want_arch "arm")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        set(_ncnn_want_arch "aarch64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        set(_ncnn_want_arch "x86_64")
    else()
        set(_ncnn_want_arch "")
    endif()

    # ---- 候选目录 ----
    set(_ncnn_candidates "")
    if(MYLIB_NCNN_ROOT)
        list(APPEND _ncnn_candidates "${MYLIB_NCNN_ROOT}")
    endif()
    file(GLOB _ncnn_dirs "${MYLIB_DEPS_DIR}/ncnn-*")
    list(APPEND _ncnn_candidates ${_ncnn_dirs})

    set(_ncnn_root "")
    set(_ncnn_rejected "")
    foreach(_c ${_ncnn_candidates})
        if(NOT EXISTS "${_c}/include/ncnn/net.h")
            continue()
        endif()

        if(MSVC)
            # MSVC 只能用自己家的 .lib；带 "linux" 字样的目录直接排除
            get_filename_component(_cname "${_c}" NAME)
            if(_cname MATCHES "linux")
                list(APPEND _ncnn_rejected "${_c} (linux)")
                continue()
            endif()
            if(EXISTS "${_c}/lib/ncnn.lib")
                set(_ncnn_root "${_c}")
                set(_ncnn_lib "${_c}/lib/ncnn.lib")
                break()
            endif()
        else()
            # GCC/MinGW：静态库或动态库都行，但架构必须对得上
            set(_ncnn_libc "")
            if(EXISTS "${_c}/lib/libncnn.a")
                set(_ncnn_libc "${_c}/lib/libncnn.a")
            elseif(EXISTS "${_c}/lib/libncnn.so")
                set(_ncnn_libc "${_c}/lib/libncnn.so")
            endif()
            if(_ncnn_libc AND _ncnn_want_arch)
                _mylib_elf_arch("${_ncnn_libc}" _a)
                if(_a STREQUAL _ncnn_want_arch)
                    set(_ncnn_root "${_c}")
                    set(_ncnn_lib "${_ncnn_libc}")
                    break()
                endif()
                list(APPEND _ncnn_rejected "${_c} (${_a})")
            endif()
        endif()
    endforeach()

    if(_ncnn_root)
        set(MYLIB_HAVE_NCNN ON)
        set(MYLIB_NCNN_ROOT_DETECTED "${_ncnn_root}")
        add_library(deps_ncnn INTERFACE)
        add_library(deps::ncnn ALIAS deps_ncnn)
        target_include_directories(deps_ncnn INTERFACE "${_ncnn_root}/include")
        target_compile_definitions(deps_ncnn INTERFACE MYLIB_HAVE_NCNN=1)
        target_link_libraries(deps_ncnn INTERFACE "${_ncnn_lib}")
        if(WIN32)
            # ncnn 在 Windows 上依赖 winsock 与系统库
            target_link_libraries(deps_ncnn INTERFACE ws2_32)
        else()
            # ncnn 的 ThreadLocalStorage 用 pthread_key_*；静态链接时必须显式带上，
            # 否则只有直接链 deps::ncnn（比如原生 API 示例）的 target 会
            # "undefined reference to pthread_key_create"。
            # 顺带把 dl 带上：ncnn 的部分后端会 dlopen。
            find_package(Threads REQUIRED)
            target_link_libraries(deps_ncnn INTERFACE Threads::Threads ${CMAKE_DL_LIBS})
        endif()
        message(STATUS "[deps] ncnn: ${_ncnn_root}")
    else()
        if(_ncnn_rejected)
            string(REPLACE ";" "\n    " _rej "${_ncnn_rejected}")
            message(STATUS "[deps] 找到了 ncnn 但架构不匹配：\n    ${_rej}")
        else()
            message(STATUS "[deps] 未找到 ncnn（需要 include/ncnn/net.h + lib/ncnn.lib）—— 跳过")
        endif()
    endif()
endif()
# ---------------------------------------------------------------- OpenCV（图像组件）
# 两条获取路径，按平台分开：
#
#   Windows：官方预编译包（MSVC ABI，opencv_worldXXX.lib），走 find_package(CONFIG)。
#   Linux / 交叉编译：自己编出来的安装树（$prefix/{include/opencv4, lib/libopencv_*.so}），
#                     **直接按模块链接，不走 find_package** —— 交叉编译时
#                     find_package 很容易捞到宿主的 OpenCV，然后链出一堆
#                     "架构不匹配/skip incompatible" 的怪错误。
#
# 交叉编译的产物由 scripts/build-opencv-armv7-petalinux2020.sh 生成，
# 放到 third_party/opencv-<版本>-armv7/ 即可被自动发现。
if(MYLIB_ENABLE_CV)
    set(MYLIB_HAVE_CV OFF)

    if(WIN32)
        # 官方 Windows 包的目录结构：
        #   opencv/build/                     <- 有 OpenCVConfig.cmake，但缺 OpenCVModules.cmake
        #   opencv/build/x64/vc16/lib/        <- 两者都有，这才是能用的 OpenCV_DIR
        # 所以优先挑"两个文件都在"的目录。
        if(NOT MYLIB_OPENCV_ROOT)
            file(GLOB _ocv_candidates
                "${MYLIB_DEPS_DIR}/opencv-*/opencv/build/*/vc*/lib"
                "${MYLIB_DEPS_DIR}/opencv-*/build/*/vc*/lib"
                "${MYLIB_DEPS_DIR}/opencv-*/opencv/build"
                "${MYLIB_DEPS_DIR}/opencv-*"
                "${MYLIB_DEPS_DIR}/opencv-*/lib/cmake/opencv4")
            # 过滤掉 Linux/交叉编译的安装树：目录里有 libopencv_core.so 就说明是
            # ELF 产物，在 Windows 上 find_package 会报
            # "IMPORTED_IMPLIB not set"，把整个 configure 拖死。
            # 板端的 OpenCV 放在 third_party/opencv-*-armv7/ 是很正常的事。
            set(_ocv_win_candidates "")
            foreach(_d ${_ocv_candidates})
                _mylib_is_elf_opencv("${_d}" _is_elf)
                if(_is_elf)
                    continue()
                endif()
                list(APPEND _ocv_win_candidates "${_d}")
            endforeach()
            set(_ocv_candidates ${_ocv_win_candidates})
            foreach(_d ${_ocv_candidates})
                if(EXISTS "${_d}/OpenCVConfig.cmake" AND EXISTS "${_d}/OpenCVModules.cmake")
                    set(MYLIB_OPENCV_ROOT "${_d}")
                    break()
                endif()
            endforeach()
            if(NOT MYLIB_OPENCV_ROOT)
                foreach(_d ${_ocv_candidates})
                    if(EXISTS "${_d}/OpenCVConfig.cmake")
                        set(MYLIB_OPENCV_ROOT "${_d}")
                        break()
                    endif()
                endforeach()
            endif()
        endif()

        # 官方包的 OpenCVConfig.cmake 依赖同目录的 OpenCVModules.cmake，
        # 且必须通过 OpenCV_DIR 走 config 模式搜索才靠得住（用 PATHS+NO_DEFAULT_PATH 会失败）。
        if(MYLIB_OPENCV_ROOT)
            set(OpenCV_DIR "${MYLIB_OPENCV_ROOT}")
        endif()
        find_package(OpenCV QUIET CONFIG)

        if(OpenCV_FOUND)
            set(MYLIB_HAVE_CV ON)
            add_library(deps_opencv INTERFACE)
            add_library(deps::opencv ALIAS deps_opencv)
            target_include_directories(deps_opencv INTERFACE ${OpenCV_INCLUDE_DIRS})
            target_link_libraries(deps_opencv INTERFACE ${OpenCV_LIBS})

            # Windows 上要把 opencv_worldXXX.dll 拷到可执行文件旁边
            if(OpenCV_DIR)
                get_filename_component(_ocv_parent "${OpenCV_DIR}" DIRECTORY)
                if(EXISTS "${_ocv_parent}/bin")
                    set(MYLIB_OPENCV_BIN_DIR "${_ocv_parent}/bin" CACHE PATH "OpenCV 运行时 DLL 目录" FORCE)
                    # 只拷 release 版：opencv_world4120.dll（debug 版是 opencv_world4120d.dll）
                    file(GLOB _ocv_all_dlls "${_ocv_parent}/bin/opencv_world[0-9]*.dll")
                    foreach(_dll ${_ocv_all_dlls})
                        if(NOT _dll MATCHES "d\\.dll$")
                            list(APPEND MYLIB_OPENCV_RUNTIME_DLLS "${_dll}")
                        endif()
                    endforeach()
                endif()
            endif()

            message(STATUS "[deps] OpenCV: ${OpenCV_VERSION} (${OpenCV_DIR})")
        else()
            message(STATUS "[deps] 未找到 OpenCV —— 跳过 cv 组件（用 -DMYLIB_OPENCV_ROOT=... 指定）")
            set(MYLIB_ENABLE_CV OFF)
        endif()
    else()
        # ---- Linux / 交叉编译：按目录 + ELF 架构挑 ----
        set(_ocv_candidates "")
        if(MYLIB_OPENCV_ROOT)
            list(APPEND _ocv_candidates "${MYLIB_OPENCV_ROOT}")
        endif()
        file(GLOB _ocv_dirs "${MYLIB_DEPS_DIR}/opencv-*")
        list(SORT _ocv_dirs)
        list(APPEND _ocv_candidates ${_ocv_dirs})

        set(_ocv_root "")
        set(_ocv_rejected "")
        foreach(_c ${_ocv_candidates})
            set(_core "")
            if(EXISTS "${_c}/lib/libopencv_core.so")
                set(_core "${_c}/lib/libopencv_core.so")
            elseif(EXISTS "${_c}/lib/libopencv_core.a")
                set(_core "${_c}/lib/libopencv_core.a")
            endif()
            if(NOT _core)
                if(EXISTS "${_c}/OpenCVConfig.cmake")
                    # 宿主上 apt 装的 OpenCV：架构天然对，交给 find_package
                    set(_ocv_root "${_c}")
                    set(_ocv_from_config ON)
                    break()
                endif()
                continue()
            endif()
            if(MYLIB_TARGET_ARCH)
                _mylib_elf_arch("${_core}" _a)
                if(NOT _a STREQUAL "${MYLIB_TARGET_ARCH}")
                    list(APPEND _ocv_rejected "${_c} (${_a} != ${MYLIB_TARGET_ARCH})")
                    continue()
                endif()
            endif()
            set(_ocv_root "${_c}")
            break()
        endforeach()

        if(_ocv_root AND _ocv_from_config)
            set(OpenCV_DIR "${_ocv_root}")
            find_package(OpenCV QUIET CONFIG)
            if(OpenCV_FOUND)
                set(MYLIB_HAVE_CV ON)
                if(OpenCV_INCLUDE_DIRS)
                    list(GET OpenCV_INCLUDE_DIRS 0 _ocv_first_inc)
                    set(MYLIB_OPENCV_INCLUDE_DIR "${_ocv_first_inc}")
                endif()
                add_library(deps_opencv INTERFACE)
                add_library(deps::opencv ALIAS deps_opencv)
                target_include_directories(deps_opencv INTERFACE ${OpenCV_INCLUDE_DIRS})
                target_link_libraries(deps_opencv INTERFACE ${OpenCV_LIBS})
                message(STATUS "[deps] OpenCV: ${OpenCV_VERSION} (${OpenCV_DIR})")
            endif()
        elseif(_ocv_root)
            set(_ocv_inc "${_ocv_root}/include/opencv4")
            if(NOT EXISTS "${_ocv_inc}/opencv2/core.hpp")
                set(_ocv_inc "${_ocv_root}/include")
            endif()

            set(_ocv_libs "")
            foreach(_m core imgproc imgcodecs)
                if(EXISTS "${_ocv_root}/lib/libopencv_${_m}.so")
                    list(APPEND _ocv_libs "${_ocv_root}/lib/libopencv_${_m}.so")
                elseif(EXISTS "${_ocv_root}/lib/libopencv_${_m}.a")
                    list(APPEND _ocv_libs "${_ocv_root}/lib/libopencv_${_m}.a")
                endif()
            endforeach()

            if(_ocv_libs AND EXISTS "${_ocv_inc}/opencv2/core.hpp")
                set(MYLIB_HAVE_CV ON)
                # 供 install(SDK) 使用：把解析出来的头目录和库目录暴露出去
                set(MYLIB_OPENCV_INCLUDE_DIR "${_ocv_inc}")
                set(MYLIB_OPENCV_LIB_DIR "${_ocv_root}/lib")
                add_library(deps_opencv INTERFACE)
                add_library(deps::opencv ALIAS deps_opencv)
                target_include_directories(deps_opencv INTERFACE "${_ocv_inc}")
                target_link_libraries(deps_opencv INTERFACE ${_ocv_libs})
                # 动态库：跑测试/程序时得能找到，给个 build rpath（板端仍然建议 LD_LIBRARY_PATH）
                set_target_properties(deps_opencv PROPERTIES
                    INTERFACE_LINK_OPTIONS "-Wl,-rpath,${_ocv_root}/lib")
                set(OpenCV_VERSION "")
                if(EXISTS "${_ocv_inc}/opencv2/core/version.hpp")
                    file(READ "${_ocv_inc}/opencv2/core/version.hpp" _ver LIMIT 2048)
                    string(REGEX MATCH "CV_VERSION_MAJOR[ \t]+([0-9]+)" _ "${_ver}")
                    set(_vmaj "${CMAKE_MATCH_1}")
                    string(REGEX MATCH "CV_VERSION_MINOR[ \t]+([0-9]+)" _ "${_ver}")
                    set(_vmin "${CMAKE_MATCH_1}")
                    string(REGEX MATCH "CV_VERSION_REVISION[ \t]+([0-9]+)" _ "${_ver}")
                    set(_vrev "${CMAKE_MATCH_1}")
                    set(OpenCV_VERSION "${_vmaj}.${_vmin}.${_vrev}")
                endif()
                message(STATUS "[deps] OpenCV: ${OpenCV_VERSION} (${_ocv_root}, 架构 ${MYLIB_TARGET_ARCH})")
            else()
                message(STATUS "[deps] ${_ocv_root} 里缺 core/imgproc/imgcodecs 或头文件 —— 跳过 cv 组件")
                set(MYLIB_ENABLE_CV OFF)
            endif()
        else()
            if(_ocv_rejected)
                string(REPLACE ";" "\n    " _rej "${_ocv_rejected}")
                message(STATUS "[deps] 找到 OpenCV 但架构不匹配：\n    ${_rej}\n"
                               "      目标: ${MYLIB_TARGET_ARCH} —— 跳过 cv 组件")
            else()
                message(STATUS "[deps] 未找到 OpenCV —— 跳过 cv 组件。"
                               "板端请先跑 scripts/build-opencv-armv7-petalinux2020.sh")
            endif()
            set(MYLIB_ENABLE_CV OFF)
        endif()
    endif()
endif()