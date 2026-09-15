cmake_minimum_required(VERSION 3.16)
project(et_mpeg2_kernel C ASM)
option(ET_DEVICE "Build ET-SoC-1 device ELF" OFF)
option(ET_CACHED_BITS "Reuse a bounded, padded slice bitreader window" ON)
option(ET_FAST_HEADERS "Extract short headers directly from the owned window" ON)
option(ET_DIRECT_BITS "Zero-copy bitreader interiors with fully bounded lookahead" ON)
option(ET_FAST_DC "Exact integer-DC/mismatch-corner reconstruction shortcut" ON)
option(ET_PREQUANT "SIMD-prescale quant matrices on qscale changes" OFF)
option(ET_PREFETCH_MC "Bounded read-only L2 prediction rectangle hints" OFF)
option(ET_EVEN_ONLY "Experimental 32-even-hart workers in a 64-hart launch" OFF)
set(ET_OPT_LEVEL 2 CACHE STRING "Device compiler optimization level")
option(ET_WIDE_MEM "Aligned RV64 memory primitives with byte tails" ON)
option(ET_IDCT_SIMD "Exact eight-column ET integer SIMD IDCT" ON)
option(ET_IDCT_ROWS "Exact eight-row integer SIMD IDCT" ON)
option(ET_IDCT_PACKED "Guarded FG32 loads and packed IDCT byte stores" ON)
option(ET_SPREAD_HARTS "Fill all minions before SMT siblings" ON)
if(ET_DEVICE)
    set(DEFAULT_MC_IMPL 3)
else()
    # Explicit host-native reference for motion; actual SIMD instruction
    # schedules are separately checked by tests/test_motion_vector.py.
    set(DEFAULT_MC_IMPL 1)
endif()
set(ET_MC_IMPL ${DEFAULT_MC_IMPL} CACHE STRING "Motion: 0 scalar, 1 SWAR, 2 paired SIMD, 3 SIMD8, 4 guarded FG32 SIMD8")
set(ET_MC_V8_STORE 0 CACHE STRING "SIMD8 store: 0 packed L1, 1 byte scatter")
set(ET_MC_LOAD 1 CACHE STRING "SWAR load: 0 bytes, 1 aligned chunks")
set(ET_MC_XY 1 CACHE STRING "SWAR xy: 0 even/odd, 1 quotient/remainder")
set(ET_BIT_WINDOW_BYTES 1024 CACHE STRING "Owned bitreader window size")
set(ADDRESS "0x8005801000" CACHE STRING "Kernel device load address")
get_filename_component(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_target(et-generate
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate.py
    WORKING_DIRECTORY ${FFMPEG_ROOT})
set(SOURCES src/decoder.c src/idct.c)
if(ET_IDCT_SIMD)
    list(APPEND SOURCES src/idct_simd.c)
endif()
if(ET_DEVICE)
    list(APPEND CMAKE_MODULE_PATH /opt/et/lib/cmake/cmake-modules)
    include(DeviceUtils)
    set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/src/linker.ld)
    add_riscv_executable(et_mpeg2_slice)
    set(TARGET et_mpeg2_slice.elf)
    target_sources(${TARGET} PRIVATE ${SOURCES} src/entry.c src/crt.S src/libc.c)
    target_compile_definitions(${TARGET} PRIVATE ET_DEVICE=1)
    target_compile_options(${TARGET} PRIVATE -O${ET_OPT_LEVEL} -ffreestanding -fno-builtin
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
    add_executable(et_memory_test tests/test_memory.c)
    target_compile_options(et_memory_test PRIVATE -UNDEBUG)
    target_compile_definitions(et_memory_test PRIVATE ET_WIDE_MEM=$<BOOL:${ET_WIDE_MEM}>)
    add_test(NAME et_memory_test COMMAND et_memory_test)
    add_executable(et_reconstruct_test tests/test_reconstruct.c)
    target_compile_options(et_reconstruct_test PRIVATE -UNDEBUG -O2)
    target_include_directories(et_reconstruct_test PRIVATE include ${FFMPEG_ROOT})
    add_test(NAME et_reconstruct_test COMMAND et_reconstruct_test)
    if(ET_IDCT_SIMD)
        target_compile_definitions(${TARGET} PRIVATE ET_IDCT_SIMD_MODEL=1)
        add_executable(et_idct_exact tests/idct_exact.c)
        target_compile_definitions(et_idct_exact PRIVATE
            ET_IDCT_ROWS=$<BOOL:${ET_IDCT_ROWS}> ET_IDCT_PACKED=$<BOOL:${ET_IDCT_PACKED}>)
        target_include_directories(et_idct_exact PRIVATE include ${FFMPEG_ROOT})
        target_link_libraries(et_idct_exact PRIVATE et_mpeg2_native)
        add_test(NAME et_idct_exact COMMAND et_idct_exact)
    endif()
endif()
target_compile_definitions(${TARGET} PRIVATE
    ET_CACHED_BITS=$<BOOL:${ET_CACHED_BITS}> ET_WIDE_MEM=$<BOOL:${ET_WIDE_MEM}>
    ET_FAST_HEADERS=$<BOOL:${ET_FAST_HEADERS}> ET_DIRECT_BITS=$<BOOL:${ET_DIRECT_BITS}>
    ET_FAST_DC=$<BOOL:${ET_FAST_DC}> ET_EVEN_ONLY=$<BOOL:${ET_EVEN_ONLY}>
    ET_PREQUANT=$<BOOL:${ET_PREQUANT}> ET_PREFETCH_MC=$<BOOL:${ET_PREFETCH_MC}>
    ET_IDCT_SIMD=$<BOOL:${ET_IDCT_SIMD}> ET_SPREAD_HARTS=$<BOOL:${ET_SPREAD_HARTS}>
    ET_IDCT_ROWS=$<BOOL:${ET_IDCT_ROWS}> ET_IDCT_PACKED=$<BOOL:${ET_IDCT_PACKED}>
    ET_MC_V8_STORE=${ET_MC_V8_STORE}
    ET_MC_IMPL=${ET_MC_IMPL} ET_MC_LOAD=${ET_MC_LOAD} ET_MC_XY=${ET_MC_XY}
    ET_BIT_WINDOW_BYTES=${ET_BIT_WINDOW_BYTES})
target_compile_features(${TARGET} PRIVATE c_std_11)
target_include_directories(${TARGET} PRIVATE include generated ${FFMPEG_ROOT})
add_dependencies(${TARGET} et-generate)
