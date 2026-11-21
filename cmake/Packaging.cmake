# =============================================================================
#  SDK 打包（CPack）
#
#  目标：`dist/` 里那些包必须**可重建**。0.1.0/0.1.1 的四个包是手工装的，
#  既没法复现、也说不清里面是哪次提交（评估报告里 C1/C2 两条问题）。
#
#  用法：
#      cmake -S . -B build/pkg -G Ninja -DCMAKE_BUILD_TYPE=Release \
#            -DMYLIB_BUILD_EXAMPLES=OFF -DMYLIB_BUILD_TESTS=OFF -DMYLIB_BUILD_TOOLS=OFF \
#            -DCMAKE_INSTALL_PREFIX=... -DMYLIB_PACKAGE_SUFFIX=win-x64-mingw
#      cmake --build build/pkg
#      cmake --build build/pkg --target package     # 或者 cpack --config build/pkg/CPackConfig.cmake
#
#  产物：my_cpp_libs-<版本>-<suffix>.zip（Windows）/ .tar.gz（Linux）
#
#  平台/工具链后缀默认按架构+编译器推，但**允许覆盖**：板端包要区分 glibc
#  （PetaLinux 2020.2 是 glibc 2.30），那个信息 CMake 推不出来，必须由构建者给。
# =============================================================================

include_guard(GLOBAL)

# ----------------------------------------------------------------------------- 后缀
if(NOT DEFINED MYLIB_PACKAGE_SUFFIX OR MYLIB_PACKAGE_SUFFIX STREQUAL "")
    set(_pkg_os "unknown")
    if(WIN32)
        set(_pkg_os "win")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_pkg_os "linux")
    endif()

    set(_pkg_arch "${CMAKE_SYSTEM_PROCESSOR}")
    if(_pkg_arch MATCHES "^(AMD64|x86_64|amd64)$")
        set(_pkg_arch "x64")
    elseif(_pkg_arch MATCHES "^(aarch64|arm64|ARM64)$")
        set(_pkg_arch "aarch64")
    elseif(_pkg_arch MATCHES "^(arm|armv7-a|armv7l)$")
        set(_pkg_arch "armv7")
    endif()

    set(_pkg_tc "")
    if(MSVC)
        set(_pkg_tc "msvc")
    elseif(MINGW)
        set(_pkg_tc "mingw")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(_pkg_tc "gcc${CMAKE_CXX_COMPILER_VERSION_MAJOR}")
    endif()

    set(MYLIB_PACKAGE_SUFFIX "${_pkg_os}-${_pkg_arch}")
    if(NOT _pkg_tc STREQUAL "")
        string(APPEND MYLIB_PACKAGE_SUFFIX "-${_pkg_tc}")
    endif()
endif()
message(STATUS "[pkg] 包名后缀: ${MYLIB_PACKAGE_SUFFIX}")

# ----------------------------------------------------------------------------- 许可证
# 再分发合规：我们的 LICENSE + 每个三方依赖的原始许可证 + 一份人可读的汇总。
# 注意 stb / pocketfft 是**头文件内嵌许可证**（没有独立 LICENSE 文件），
# 它们的代码被静态链进了 libmy_cpp_libs_image.a / _dsp.a，所以把源文件本身
# 放进 licenses/sources/ —— 许可证原文就在文件头里，原样保留最不容易出错。
if(MYLIB_INSTALL)
    set(_lic_dir "${CMAKE_INSTALL_DATADIR}/licenses/my_cpp_libs")

    install(FILES "${PROJECT_SOURCE_DIR}/LICENSE"
            DESTINATION "${_lic_dir}" RENAME "my_cpp_libs-LICENSE")

    if(EXISTS "${PROJECT_SOURCE_DIR}/docs/THIRD_PARTY_NOTICES.md")
        install(FILES "${PROJECT_SOURCE_DIR}/docs/THIRD_PARTY_NOTICES.md"
                DESTINATION "${_lic_dir}")
    endif()

    # 装一个三方许可证：从候选路径里挑第一个存在的。
    # 找不到就是发布缺陷（README 里"出现 [pkg] 找不到许可证 就不要发布"），
    # 因此这里直接 FAIL，而不是只打一行容易被忽略的 WARNING。
    function(_mylib_install_license _dest_name)
        foreach(_cand ${ARGN})
            if(EXISTS "${_cand}")
                install(FILES "${_cand}" DESTINATION "${_lic_dir}" RENAME "${_dest_name}")
                return()
            endif()
        endforeach()
        # 兜底：随仓库提交的许可证副本（用于自编的 armv7 ORT 这类没带 LICENSE 的产物）
        set(_fallback "${PROJECT_SOURCE_DIR}/cmake/licenses/${_dest_name}")
        if(EXISTS "${_fallback}")
            install(FILES "${_fallback}" DESTINATION "${_lic_dir}" RENAME "${_dest_name}")
            return()
        endif()
        message(FATAL_ERROR
            "[pkg] 找不到许可证 ${_dest_name}（候选: ${ARGN}，兜底: ${_fallback}）。"
            "缺许可证的包不应发布；补齐候选路径或 cmake/licenses/ 下的兜底文件。")
    endfunction()

    if(TARGET mylib_log)
        _mylib_install_license("spdlog-LICENSE"
            "${MYLIB_DEP_spdlog-1.17.0}/LICENSE"
            "${PROJECT_SOURCE_DIR}/third_party/spdlog-1.17.0/LICENSE")
        _mylib_install_license("fmt-LICENSE.rst"
            "${MYLIB_DEP_spdlog-1.17.0}/include/spdlog/fmt/bundled/fmt.license.rst"
            "${PROJECT_SOURCE_DIR}/third_party/spdlog-1.17.0/include/spdlog/fmt/bundled/fmt.license.rst")
    endif()

    if(TARGET mylib_json)
        _mylib_install_license("nlohmann-json-LICENSE.MIT"
            "${MYLIB_DEP_nlohmann-json-3.12.0}/LICENSE.MIT"
            "${PROJECT_SOURCE_DIR}/third_party/nlohmann-json-3.12.0/LICENSE.MIT")
    endif()

    if(TARGET mylib_math)
        _mylib_install_license("eigen-COPYING.MPL2"
            "${MYLIB_DEP_eigen-5.0.0}/COPYING.MPL2"
            "${PROJECT_SOURCE_DIR}/third_party/eigen-5.0.0/COPYING.MPL2")
        _mylib_install_license("eigen-COPYING.README"
            "${MYLIB_DEP_eigen-5.0.0}/COPYING.README"
            "${PROJECT_SOURCE_DIR}/third_party/eigen-5.0.0/COPYING.README")
    endif()

    if(TARGET mylib_http)
        _mylib_install_license("cpp-httplib-LICENSE"
            "${MYLIB_DEP_httplib-0.66.0}/LICENSE"
            "${PROJECT_SOURCE_DIR}/third_party/httplib-0.66.0/LICENSE")
    endif()

    if(TARGET mylib_image)
        # stb 没有独立 LICENSE 文件，许可证在头文件顶部
        _mylib_install_license("sources/stb_image.h"
            "${MYLIB_DEP_stb-image-2.32}/stb_image.h"
            "${PROJECT_SOURCE_DIR}/third_party/stb-image-2.32/stb_image.h")
        _mylib_install_license("sources/stb_image_write.h"
            "${MYLIB_DEP_stb-image-2.32}/stb_image_write.h"
            "${PROJECT_SOURCE_DIR}/third_party/stb-image-2.32/stb_image_write.h")
    endif()

    if(TARGET mylib_dsp)
        # pocketfft 同理：单头文件，BSD-3 声明在文件头
        _mylib_install_license("sources/pocketfft_hdronly.h"
            "${MYLIB_DEP_pocketfft-4d865cb}/pocketfft_hdronly.h"
            "${PROJECT_SOURCE_DIR}/third_party/pocketfft-4d865cb/pocketfft_hdronly.h")
    endif()

    if(TARGET deps::onnxruntime AND MYLIB_ORT_ROOT_DETECTED)
        _mylib_install_license("onnxruntime-LICENSE"
            "${MYLIB_ORT_ROOT_DETECTED}/LICENSE"
            "${MYLIB_ORT_ROOT_DETECTED}/LICENSE.txt")
        _mylib_install_license("onnxruntime-ThirdPartyNotices.txt"
            "${MYLIB_ORT_ROOT_DETECTED}/ThirdPartyNotices.txt"
            "${MYLIB_ORT_ROOT_DETECTED}/ThirdPartyNotices")
    endif()

    if(TARGET deps::ncnn AND MYLIB_NCNN_ROOT_DETECTED)
        _mylib_install_license("ncnn-LICENSE.txt"
            "${MYLIB_NCNN_ROOT_DETECTED}/LICENSE.txt"
            "${MYLIB_NCNN_ROOT_DETECTED}/LICENSE"
            "${PROJECT_SOURCE_DIR}/third_party/ncnn-src/LICENSE.txt")
    endif()

    if(TARGET deps::opencv)
        _mylib_install_license("opencv-LICENSE"
            "${PROJECT_SOURCE_DIR}/third_party/opencv-4.12.0/opencv/build/LICENSE"
            "${PROJECT_SOURCE_DIR}/third_party/opencv-4.12.0/opencv/LICENSE"
            "${MYLIB_OPENCV_ROOT}/LICENSE"
            "/usr/share/doc/libopencv-dev/copyright")
    endif()
endif()

# ----------------------------------------------------------------------------- CPack
set(CPACK_PACKAGE_NAME            "my_cpp_libs")
set(CPACK_PACKAGE_VENDOR          "dowdyboy")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "跨平台 C++17 三方库统一封装层（core/io/time/thread/log/json/math/infer/preprocess/image/dsp/ml/net/serial/http/cv）")
set(CPACK_PACKAGE_VERSION         "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME       "my_cpp_libs-${PROJECT_VERSION}-${MYLIB_PACKAGE_SUFFIX}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "my_cpp_libs-${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT         "dowdyboy <39474100+dowdyboy@users.noreply.github.com>")
set(CPACK_RESOURCE_FILE_LICENSE   "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_STRIP_FILES             OFF)   # 保留调试信息由调用方用 CMAKE_BUILD_TYPE 控制

# 把构建身份写进包内，方便事后对账（"这个包到底是哪次 commit 编的"）
set(CPACK_PACKAGE_DESCRIPTION
    "my_cpp_libs ${PROJECT_VERSION}
git commit : ${MYLIB_GIT_COMMIT}
compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}
system     : ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}
build type : ${MYLIB_BUILD_TYPE_STR}
components : ${_components_desc}")

if(WIN32)
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TGZ")
endif()

# 源目录里的杂项不要进包（install 规则已经决定了内容，这里是兜底）
set(CPACK_SOURCE_IGNORE_FILES "/build/;/third_party/;/dist/;/\\.git/;~$")

include(CPack)
