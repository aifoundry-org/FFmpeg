# SPDX-License-Identifier: LGPL-2.1-or-later
cmake_minimum_required(VERSION 3.16)
project(etaac_prototype C CXX ASM)
option(ET_DEVICE "Build isolated AAC device ELF (never executes it)" OFF)
set(ADDRESS "0x8005801000" CACHE STRING "Device load address")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(DSP dsp/etaac_synth.c)
if(ET_DEVICE)
    list(APPEND CMAKE_MODULE_PATH /opt/et/lib/cmake/cmake-modules)
    include(DeviceUtils)
    set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/../et-kernels/src/linker.ld)
    add_riscv_executable(et_aac_synth)
    set(TARGET et_aac_synth.elf)
    target_sources(${TARGET} PRIVATE ${DSP} kernel.c
        ../et-kernels/src/crt.S ../et-kernels/src/libc.c)
    target_compile_definitions(${TARGET} PRIVATE ET_DEVICE=1 ET_WIDE_MEM=1)
    target_compile_options(${TARGET} PRIVATE -O2 -ffreestanding -fno-builtin
        -fno-zero-initialized-in-bss -mcmodel=medany -mabi=lp64f -march=rv64imf
        -mstrict-align -ffunction-sections -fdata-sections -fno-tree-vectorize
        -ffp-contract=off -fno-fast-math -fno-strict-aliasing
        -Wno-conversion -Wno-sign-conversion -Wno-undef -Wno-cast-align
        -Wno-unused-parameter -Wno-missing-prototypes -Wno-missing-declarations)
    target_link_options(${TARGET} PRIVATE -march=rv64imf -mabi=lp64f
        -ffp-contract=off -Wl,--defsym=BASE_ADDRESS=${ADDRESS} -Wl,--entry=_start)
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/check-device.py
          $<TARGET_FILE:${TARGET}> /opt/et/bin/riscv64-unknown-elf-)
else()
    add_library(etaac_native STATIC ${DSP} kernel.c)
    target_compile_options(etaac_native PRIVATE -O2 -Wall -Wextra -ffp-contract=off
        -fno-fast-math -fno-strict-aliasing)
    include(CTest)
    set(ETAAC_CPU_BUILD "${CMAKE_CURRENT_SOURCE_DIR}/../build-et/aac-prototype/cpu" CACHE PATH "Pinned CPU oracle build")
    if(EXISTS "${ETAAC_CPU_BUILD}/libavcodec/libavcodec.a")
        add_executable(etaac-ffmpeg-exact dsp/tests/test_etaac_ffmpeg.c)
        target_include_directories(etaac-ffmpeg-exact PRIVATE dsp ${ETAAC_CPU_BUILD} ${CMAKE_CURRENT_SOURCE_DIR}/..)
        target_compile_options(etaac-ffmpeg-exact PRIVATE -UNDEBUG -ffp-contract=off)
        target_link_libraries(etaac-ffmpeg-exact PRIVATE etaac_native
            ${ETAAC_CPU_BUILD}/libavcodec/libavcodec.a ${ETAAC_CPU_BUILD}/libavutil/libavutil.a m pthread)
        add_test(NAME etaac-ffmpeg-exact COMMAND etaac-ffmpeg-exact)
    endif()
    add_executable(etaac-replay-native replay-native.c)
    target_link_libraries(etaac-replay-native PRIVATE etaac_native)
    target_compile_options(etaac-replay-native PRIVATE -ffp-contract=off)
    add_executable(etaac-native-test tests.c)
    target_link_libraries(etaac-native-test PRIVATE etaac_native)
    target_compile_options(etaac-native-test PRIVATE -UNDEBUG -ffp-contract=off)
    add_test(NAME etaac-native-test COMMAND etaac-native-test)
    option(ETAAC_BUILD_RUNTIME "Build SDK host runner; build does not open device" ON)
    if(ETAAC_BUILD_RUNTIME)
        add_subdirectory(runtime)
        add_executable(etaac-run runner.c)
        target_link_libraries(etaac-run PRIVATE etaac_native etaac_runtime m)
        set_target_properties(etaac-run PROPERTIES LINKER_LANGUAGE CXX)
        target_compile_options(etaac-run PRIVATE -O2 -Wall -Wextra -ffp-contract=off)
    endif()
endif()
