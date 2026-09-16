# SPDX-License-Identifier: LGPL-2.1-or-later
cmake_minimum_required(VERSION 3.16)
project(etaac_runtime VERSION 1.0.0 LANGUAGES C CXX)
include(GNUInstallDirs)
include(CTest)

# This is deliberately an independent static adapter.  Do not link it into an
# executable that also contains libavcodec's video ET runtime symbols.
set(ET_SDK "$ENV{FF_ET_SDK}" CACHE PATH "ET SDK installation")
if(NOT ET_SDK)
    set(ET_SDK /opt/et)
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(ETAAC_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(ETAAC_GENERATED_RUNTIME "${ETAAC_GENERATED_DIR}/et_runtime.cpp")
add_custom_command(
    OUTPUT "${ETAAC_GENERATED_RUNTIME}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_runtime.py"
            --source "${CMAKE_CURRENT_SOURCE_DIR}/../../libavcodec/et_runtime.cpp"
            --output "${ETAAC_GENERATED_RUNTIME}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/generate_runtime.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../libavcodec/et_runtime.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/etaac_runtime_private.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/../protocol.h"
    COMMENT "Generating pinned isolated AAC ET runtime"
    VERBATIM)
add_custom_target(etaac-runtime-generate DEPENDS "${ETAAC_GENERATED_RUNTIME}")

option(ETAAC_RUNTIME_BUILD_ADAPTER "Build the SDK-linked AAC adapter" ON)
if(ETAAC_RUNTIME_BUILD_ADAPTER)
    list(PREPEND CMAKE_PREFIX_PATH "${ET_SDK}" "${ET_SDK}/lib/cmake")
    list(PREPEND CMAKE_MODULE_PATH "${ET_SDK}/lib/cmake" "${ET_SDK}/lib/cmake/cmake-modules")
    find_package(runtime CONFIG REQUIRED)
    find_package(deviceLayer CONFIG REQUIRED)

    add_library(etaac_runtime STATIC "${ETAAC_GENERATED_RUNTIME}")
    add_dependencies(etaac_runtime etaac-runtime-generate)
    target_compile_features(etaac_runtime PRIVATE cxx_std_17)
    set_target_properties(etaac_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(etaac_runtime PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(etaac_runtime
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/etaac>)
    target_link_libraries(etaac_runtime PRIVATE runtime::etrt_static deviceLayer::deviceLayer)

    install(TARGETS etaac_runtime ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES etaac_runtime.h ../protocol.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/etaac)
endif()

if(BUILD_TESTING)
    # Header shadowing supplies a no-device SDK.  This target only exercises
    # validation and failure paths; it never constructs the actual SDK runtime.
    add_executable(etaac-runtime-unit tests/runtime_test.cpp "${ETAAC_GENERATED_RUNTIME}")
    add_dependencies(etaac-runtime-unit etaac-runtime-generate)
    target_compile_features(etaac-runtime-unit PRIVATE cxx_std_17)
    target_compile_options(etaac-runtime-unit PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(etaac-runtime-unit PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/mock"
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/..")
    add_test(NAME etaac-runtime-unit COMMAND etaac-runtime-unit)
endif()
