cmake_minimum_required(VERSION 3.24)
project(mxl-color-corrector VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CC_BUILD_TESTS "Build unit tests" ON)
option(CC_WITH_MXL "Fetch and link MXL release/v1.1" ON)

find_package(Threads REQUIRED)

set(CC_CORE_SOURCES
    src/v210.cpp
    src/color/controls.cpp
    src/color/process.cpp
    src/color/process_avx2.cpp
    src/util/sha1.cpp
    src/util/uuid.cpp
    src/util/json.cpp
    src/util/log.cpp
    src/config.cpp
    src/store.cpp
    src/metrics.cpp
    src/preview.cpp
    src/http.cpp
    src/api.cpp
    src/nmos.cpp
)

add_library(cc_core STATIC ${CC_CORE_SOURCES})
target_include_directories(cc_core PUBLIC src third_party)
target_link_libraries(cc_core PUBLIC Threads::Threads)
target_compile_options(cc_core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set_source_files_properties(src/color/process_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2")
endif()

if(CC_BUILD_TESTS)
    enable_testing()
    add_executable(unit-tests
        tests/unit/test_main.cpp
        tests/unit/test_color.cpp
        tests/unit/test_config.cpp
        tests/unit/test_api.cpp
    )
    target_include_directories(unit-tests PRIVATE src third_party)
    target_link_libraries(unit-tests PRIVATE cc_core)
    target_compile_options(unit-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
    add_test(NAME unit-tests COMMAND unit-tests)
endif()

if(CC_WITH_MXL)
    include(FetchContent)
    set(FETCHCONTENT_QUIET OFF)
    set(MXL_TAG 218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7)
    FetchContent_Declare(mxl_src
        GIT_REPOSITORY https://github.com/dmf-mxl/mxl.git
        GIT_TAG ${MXL_TAG}
    )
    FetchContent_Declare(fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG 11.1.4
        GIT_SHALLOW TRUE
    )
    FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.15.1
        GIT_SHALLOW TRUE
    )
    set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(stduuid
        GIT_REPOSITORY https://github.com/mariusbancila/stduuid.git
        GIT_TAG v1.2.3
        GIT_SHALLOW TRUE
    )
    set(UUID_SYSTEM_GENERATOR OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(picojson
        URL https://github.com/kazuho/picojson/archive/refs/tags/v1.3.0.tar.gz
    )
    FetchContent_MakeAvailable(fmt spdlog stduuid picojson)
    FetchContent_GetProperties(mxl_src)
    if(NOT mxl_src_POPULATED)
        FetchContent_Populate(mxl_src)
    endif()

    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/pico_inc/picojson)
    file(CREATE_LINK ${picojson_SOURCE_DIR}/picojson.h ${CMAKE_BINARY_DIR}/pico_inc/picojson/picojson.h SYMBOLIC)
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/pico_wrap/picojson)
    file(WRITE ${CMAKE_BINARY_DIR}/pico_wrap/picojson/wrapper.h
"#pragma once
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored \"-Wmaybe-uninitialized\"
#include <picojson/picojson.h>
#pragma GCC diagnostic pop
")
    add_library(picojson_lib INTERFACE)
    target_include_directories(picojson_lib INTERFACE ${CMAKE_BINARY_DIR}/pico_wrap ${CMAKE_BINARY_DIR}/pico_inc)
    add_library(picojson::picojson ALIAS picojson_lib)

    file(WRITE ${CMAKE_BINARY_DIR}/mxl_version/mxl/version.h
"#pragma once
#define MXL_VERSION_MAJOR 1
#define MXL_VERSION_MINOR 1
#define MXL_VERSION_PATCH 0
#define MXL_VERSION_BUILD 0
#define MXL_VERSION_FULL \"1.1.0+0 g218ddaa0a08c\"
")

    set(MXL_INTERNAL ${mxl_src_SOURCE_DIR}/lib/internal)
    add_library(mxl STATIC
        ${mxl_src_SOURCE_DIR}/lib/src/flow.cpp
        ${mxl_src_SOURCE_DIR}/lib/src/mxl.cpp
        ${mxl_src_SOURCE_DIR}/lib/src/time.cpp
        ${MXL_INTERNAL}/src/DomainWatcher.cpp
        ${MXL_INTERNAL}/src/FlowData.cpp
        ${MXL_INTERNAL}/src/FlowInfo.cpp
        ${MXL_INTERNAL}/src/FlowIoFactory.cpp
        ${MXL_INTERNAL}/src/FlowManager.cpp
        ${MXL_INTERNAL}/src/FlowOptionsParser.cpp
        ${MXL_INTERNAL}/src/FlowParser.cpp
        ${MXL_INTERNAL}/src/FlowReader.cpp
        ${MXL_INTERNAL}/src/FlowSynchronizationGroup.cpp
        ${MXL_INTERNAL}/src/FlowWriter.cpp
        ${MXL_INTERNAL}/src/Instance.cpp
        ${MXL_INTERNAL}/src/Logging.cpp
        ${MXL_INTERNAL}/src/MediaUtils.cpp
        ${MXL_INTERNAL}/src/PathUtils.cpp
        ${MXL_INTERNAL}/src/PosixContinuousFlowReader.cpp
        ${MXL_INTERNAL}/src/PosixContinuousFlowWriter.cpp
        ${MXL_INTERNAL}/src/PosixDiscreteFlowReader.cpp
        ${MXL_INTERNAL}/src/PosixDiscreteFlowWriter.cpp
        ${MXL_INTERNAL}/src/PosixFlowIoFactory.cpp
        ${MXL_INTERNAL}/src/SharedMemory.cpp
        ${MXL_INTERNAL}/src/Sync.cpp
        ${MXL_INTERNAL}/src/Thread.cpp
        ${MXL_INTERNAL}/src/Time.cpp
        ${MXL_INTERNAL}/src/Timing.cpp
    )
    target_compile_features(mxl PUBLIC cxx_std_20)
    target_include_directories(mxl
        PUBLIC
            ${mxl_src_SOURCE_DIR}/lib/include
            ${CMAKE_BINARY_DIR}/mxl_version
        PRIVATE
            ${MXL_INTERNAL}/include
    )
    target_link_libraries(mxl PUBLIC fmt::fmt spdlog::spdlog stduuid picojson_lib)
    target_compile_definitions(mxl PRIVATE SPDLOG_FMT_EXTERNAL)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_definitions(mxl PRIVATE NDEBUG)
    endif()
    add_library(mxl::mxl ALIAS mxl)

    set(WEBUI_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/webui.hpp)
    set(WEBUI_HTML ${CMAKE_CURRENT_SOURCE_DIR}/web/dist/index.html)
    if(EXISTS ${WEBUI_HTML})
        add_custom_command(
            OUTPUT ${WEBUI_GENERATED}
            COMMAND ${CMAKE_COMMAND} -DINPUT=${WEBUI_HTML} -DOUTPUT=${WEBUI_GENERATED}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
            DEPENDS ${WEBUI_HTML} cmake/EmbedFile.cmake
            COMMENT "Embedding web UI"
            VERBATIM
        )
    else()
        file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)
        file(WRITE ${WEBUI_GENERATED}
"#pragma once
#include <string_view>
namespace cc::webui {
inline std::string_view indexHtml() {
    return \"<!doctype html><title>mxl-color-corrector</title><p>Web UI was not built. Run npm run build in web/.</p>\";
}
}
")
    endif()

    add_executable(mxl-color-corrector src/main.cpp src/engine.cpp ${WEBUI_GENERATED})
    target_include_directories(mxl-color-corrector PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_link_libraries(mxl-color-corrector PRIVATE cc_core mxl::mxl)
    target_compile_options(mxl-color-corrector PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_compile_definitions(mxl-color-corrector PRIVATE CC_HAS_UI=1)

    add_executable(mxl-cc-pattern tests/tools/pattern_writer.cpp)
    target_link_libraries(mxl-cc-pattern PRIVATE cc_core mxl::mxl)
    target_compile_features(mxl-cc-pattern PRIVATE cxx_std_20)

    add_executable(mxl-cc-sample tests/tools/sample_flow.cpp)
    target_link_libraries(mxl-cc-sample PRIVATE cc_core mxl::mxl)
    target_compile_features(mxl-cc-sample PRIVATE cxx_std_20)

    install(TARGETS mxl-color-corrector mxl-cc-pattern mxl-cc-sample RUNTIME DESTINATION bin)
endif()

add_executable(mxl-cc-bench tests/tools/bench.cpp)
target_link_libraries(mxl-cc-bench PRIVATE cc_core)
target_compile_features(mxl-cc-bench PRIVATE cxx_std_20)
