# SPDX-License-Identifier: LGPL-2.1-or-later
cmake_minimum_required(VERSION 3.16)
project(etsoc_runtime VERSION 1.0.0 LANGUAGES C CXX)
include(GNUInstallDirs)

set(ET_SDK "$ENV{FF_ET_SDK}" CACHE PATH "ET SDK installation")
if(NOT ET_SDK)
    set(ET_SDK /opt/et)
endif()
list(PREPEND CMAKE_PREFIX_PATH "${ET_SDK}" "${ET_SDK}/lib/cmake")
list(PREPEND CMAKE_MODULE_PATH "${ET_SDK}/lib/cmake" "${ET_SDK}/lib/cmake/cmake-modules")
find_package(runtime CONFIG REQUIRED)
find_package(deviceLayer CONFIG REQUIRED)

add_library(etsoc STATIC ../libavcodec/et_runtime.cpp)
target_compile_features(etsoc PRIVATE cxx_std_17)
set_target_properties(etsoc PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(etsoc PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(etsoc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../libavcodec>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/etsoc>)
target_link_libraries(etsoc PRIVATE runtime::etrt_static deviceLayer::deviceLayer)

# The SDK runtime/device-layer are static; their dependencies must be present
# even for plain pkg-config --libs (FFmpeg does not always pass --static).
# Keep the SDK's mutually dependent static archives in a linker group.
set(ET_PC_LIBS "-L${ET_SDK}/lib -Wl,-rpath,${ET_SDK}/lib -Wl,--start-group -letrt_static -ldeviceLayer -lsw-sysemu -ldebug -llogging -lthreadPool -lactionList -Wl,--end-group -leasy_profiler -lcap -lg3log -llz4 -lglog -lpthread -lstdc++ -lm -ldl")
configure_file(etsoc.pc.in etsoc.pc @ONLY)

install(TARGETS etsoc ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ../libavcodec/et_runtime.h ../libavcodec/et_mpeg2_protocol.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/etsoc)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/etsoc.pc"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

option(ET_RUNTIME_BUILD_SMOKE "Build explicitly invoked probe/DMA smoke tool (never auto-launches hardware)" ON)
if(ET_RUNTIME_BUILD_SMOKE)
    add_executable(et-runtime-smoke smoke.c)
    set_target_properties(et-runtime-smoke PROPERTIES LINKER_LANGUAGE CXX)
    target_link_libraries(et-runtime-smoke PRIVATE etsoc)
endif()

include(CTest)
if(BUILD_TESTING)
    # This target shadows only the SDK headers, never uses a device, and can
    # inject faults which would be inappropriate to exercise on real silicon.
    add_executable(et-runtime-unit tests/runtime_test.cpp ../libavcodec/et_runtime.cpp)
    target_compile_features(et-runtime-unit PRIVATE cxx_std_17)
    target_include_directories(et-runtime-unit PRIVATE tests/mock ../libavcodec)
    target_compile_options(et-runtime-unit PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME et-runtime-unit COMMAND et-runtime-unit)
endif()
