cmake_minimum_required(VERSION 3.16)
project(et_mpeg2_kernel C ASM)
option(ET_DEVICE "Build ET-SoC-1 device ELF" OFF)
set(ADDRESS "0x8005801000" CACHE STRING "Kernel device load address")
get_filename_component(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_target(et-generate
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate.py
    WORKING_DIRECTORY ${FFMPEG_ROOT})
set(SOURCES src/decoder.c src/idct.c)
if(ET_DEVICE)
    list(APPEND CMAKE_MODULE_PATH /opt/et/lib/cmake/cmake-modules)
    include(DeviceUtils)
    set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/src/linker.ld)
    add_riscv_executable(et_mpeg2_slice)
    set(TARGET et_mpeg2_slice.elf)
    target_sources(${TARGET} PRIVATE ${SOURCES} src/entry.c src/crt.S src/libc.c)
    target_compile_definitions(${TARGET} PRIVATE ET_DEVICE=1)
    target_compile_options(${TARGET} PRIVATE -O2 -ffreestanding -fno-builtin
        -fno-zero-initialized-in-bss -mcmodel=medany -mabi=lp64f -march=rv64imf
        -mstrict-align -ffunction-sections -fdata-sections -fno-tree-vectorize)
    target_link_options(${TARGET} PRIVATE -march=rv64imf -mabi=lp64f
        -Wl,--defsym=BASE_ADDRESS=${ADDRESS} -Wl,--entry=_start)
    # SDK warning policy applies to its own code; FFmpeg's unchanged generic
    # headers intentionally use narrowing casts and undefined arch macros.
    target_compile_options(${TARGET} PRIVATE -Wno-conversion -Wno-sign-conversion
        -Wno-undef -Wno-cast-align -Wno-unused-parameter -Wno-missing-prototypes
        -Wno-missing-declarations)
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-device.py
            $<TARGET_FILE:${TARGET}> /opt/et/bin/riscv64-unknown-elf-)

else()
    add_library(et_mpeg2_native SHARED ${SOURCES})
    set(TARGET et_mpeg2_native)
    target_compile_options(${TARGET} PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
    add_executable(et_mpeg2_test tests/test_decode.c)
    # Assertions are the test oracle; never compile them away in Release.
    target_compile_options(et_mpeg2_test PRIVATE -UNDEBUG)
    target_include_directories(et_mpeg2_test PRIVATE include ${FFMPEG_ROOT})
    target_link_libraries(et_mpeg2_test PRIVATE et_mpeg2_native)
    enable_testing()
    add_test(NAME et_mpeg2_native COMMAND et_mpeg2_test)
endif()
target_compile_features(${TARGET} PRIVATE c_std_11)
target_include_directories(${TARGET} PRIVATE include generated ${FFMPEG_ROOT})
add_dependencies(${TARGET} et-generate)
