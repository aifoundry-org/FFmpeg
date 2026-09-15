/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Native-only exact DC/corner shortcut test; see reconstruct.md.
 * The oracle is FFmpeg's unchanged simple_idct_template.c, not a DCT model.
 */
#define _GNU_SOURCE
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "config.h"
#include "libavutil/intreadwrite.h"
#include "libavcodec/mathops.h"
#define ff_simple_idct_put_int16_8bit oracle_put
#define ff_simple_idct_add_int16_8bit oracle_add
#define ff_simple_idct_int16_8bit oracle_idct
#define IN_IDCT_DEPTH 16
#define BIT_DEPTH 8
#include "libavcodec/simple_idct_template.c"
#undef ff_simple_idct_put_int16_8bit
#undef ff_simple_idct_add_int16_8bit
#undef ff_simple_idct_int16_8bit
#ifdef ET_DEVICE
#error This fixture must be compiled natively with ET_DEVICE undefined
#endif
#include "../src/reconstruct.h"

static uint64_t accepted, refused, modeled, raw_idcts;
static int case_dc, case_corner, case_pred, case_add;
static ptrdiff_t case_stride;
#define CHECK(c) do { if (!(c)) fail(#c, __LINE__); } while (0)
static void fail(const char *what, int line)
{
    fprintf(stderr, "FAIL line %d: %s dc=%d corner=%d pred=%d add=%d stride=%td\n",
            line, what, case_dc, case_corner, case_pred, case_add, case_stride);
    exit(1);
}

/* A separate native model of the Add assembly's byte gather, unsigned mask,
 * modulo-32-bit integer add, signed saturation, and two packed word stores.
 * This checks the intended lane arithmetic/packing, NOT ET ISA execution,
 * gather/scatter configuration words, register constraints, or mask restore.
 */
static void add_lane_model(uint8_t *dst, ptrdiff_t stride, int dc)
{
    if (!dc) return;
    for (unsigned y = 0; y < 8; ++y) {
        uint32_t lane[8], packed[2] = {0, 0};
        for (unsigned x = 0; x < 8; ++x) {
            lane[x] = ((uint32_t)dst[x] & 255) + (uint32_t)dc;
            int64_t signed_lane = lane[x] <= INT32_MAX ? (int64_t)lane[x] :
                                  (int64_t)lane[x] - INT64_C(4294967296);
            lane[x] = signed_lane < 0 ? 0 : signed_lane > 255 ? 255 :
                      (uint32_t)signed_lane;
            packed[x / 4] |= lane[x] << (8 * (x % 4));
        }
        memcpy(dst, packed, 8);
        if (y != 7) dst += stride;
    }
    ++modeled;
}

static void compare_case(int dc, int corner, int pred, ptrdiff_t stride,
                         int add, int varying)
{
    _Alignas(64) int16_t block[96], copy[64], original[96];
    _Alignas(64) uint8_t actual[1152], expected[1152], model[1152];
    /* Independently cycle all legal source/destination residues modulo 64. */
    unsigned block_offset = 4 * (accepted & 7);
    const unsigned offset = 64 + 8 * ((accepted >> 3) & 7) +
                            (stride < 0 ? (unsigned)(-stride) * 7 : 0);
    case_dc = dc; case_corner = corner; case_pred = pred;
    case_stride = stride; case_add = add;
    memset(block, 0xa6, sizeof(block));
    memset(block + block_offset, 0, 128);
    block[block_offset] = dc; block[block_offset + 63] = corner;
    memcpy(original, block, sizeof(block));
    memcpy(copy, block + block_offset, 128);
    memset(actual, 0x93, sizeof(actual));
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x)
            actual[offset + (ptrdiff_t)y * stride + x] =
                (uint8_t)(pred + (varying ? y * 37 + x * 17 : 0));
    memcpy(expected, actual, sizeof(actual));
    if (add) memcpy(model, actual, sizeof(actual));
    if (add) oracle_add(expected + offset, stride, copy);
    else oracle_put(expected + offset, stride, copy);
    CHECK(et_reconstruct_dc(actual + offset, stride, block + block_offset, add) == 1);
    CHECK(!memcmp(actual, expected, sizeof(actual)));
    CHECK(!memcmp(block, original, sizeof(block)));
    if (add) {
        add_lane_model(model + offset, stride, dc / 8);
        CHECK(!memcmp(model, expected, sizeof(model)));
    }
    ++accepted;
}

static void raw_transform(void)
{
    for (int dc = -2048; dc <= 2040; dc += 8)
        for (int corner = -1; corner <= 1; ++corner) {
            _Alignas(64) int16_t block[64] = {0};
            case_dc = block[0] = dc;
            case_corner = block[63] = corner;
            oracle_idct(block);
            for (unsigned i = 0; i < 64; ++i) CHECK(block[i] == dc / 8);
            ++raw_idcts;
        }
}

static void exhaustive(void)
{
    static const ptrdiff_t strides[] = {8, 16, 24, 64, 128, -8, -64, -128, 0};
    for (int dc = -2048; dc <= 2040; dc += 8)
        for (int corner = -1; corner <= 1; ++corner)
            for (unsigned s = 0; s < sizeof(strides) / sizeof(strides[0]); ++s) {
                /* Intra does not depend on prediction, but run both modes for
                 * every prediction so the coverage claim is literal. */
                for (int pred = 0; pred <= 255; ++pred)
                    for (int add = 0; add <= 1; ++add)
                        compare_case(dc, corner, pred, strides[s], add, 0);
                for (int add = 0; add <= 1; ++add)
                    compare_case(dc, corner, 133, strides[s], add, 1);
            }
}

static void reject_case(const void *bytes, unsigned dst_offset, ptrdiff_t stride)
{
    _Alignas(64) uint8_t dst[1152], before[1152];
    uint8_t block_before[128];
    memcpy(block_before, bytes, 128);
    for (int add = 0; add <= 1; ++add) {
        case_add = add; case_stride = stride;
        for (unsigned i = 0; i < sizeof(dst); ++i) dst[i] = (uint8_t)(i * 37);
        memcpy(before, dst, sizeof(dst));
        CHECK(et_reconstruct_dc(dst + 64 + dst_offset, stride,
                                 (const int16_t *)bytes, add) == 0);
        CHECK(!memcmp(dst, before, sizeof(dst)));
        CHECK(!memcmp(bytes, block_before, 128));
        ++refused;
    }
}

static void refusals(void)
{
    _Alignas(64) int16_t block[64] = {0};
    _Alignas(64) uint8_t unaligned[144];
    static const int16_t ac[] = {INT16_MIN, -257, -1, 1, 256, INT16_MAX};
    /* Every int16 DC outside the advertised accepted set. */
    for (int dc = INT16_MIN; dc <= INT16_MAX; ++dc) {
        if (dc >= -2048 && dc <= 2040 && dc % 8 == 0) continue;
        case_dc = block[0] = dc;
        reject_case(block, 0, 128);
    }
    case_dc = block[0] = 8;
    for (int corner = INT16_MIN; corner <= INT16_MAX; ++corner) {
        if (corner >= -1 && corner <= 1) continue;
        case_corner = block[63] = corner;
        reject_case(block, 0, 128);
    }
    case_corner = block[63] = 1;
    for (unsigned k = 1; k < 63; ++k)
        for (unsigned v = 0; v < sizeof(ac) / sizeof(ac[0]); ++v) {
            block[k] = ac[v];
            reject_case(block, 0, 128);
            block[k] = 0;
        }
    for (unsigned misalign = 1; misalign < 8; ++misalign) {
        reject_case(block, misalign, 128);
        reject_case(block, 0, 128 + misalign);
        memset(unaligned, 0xb5, sizeof(unaligned));
        memcpy(unaligned + misalign, block, 128);
        reject_case(unaligned + misalign, 0, 128);
    }
}

static uint8_t *map_pages(size_t length)
{
    uint8_t *p = mmap(NULL, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    return p;
}

static void guard_pages(void)
{
    long page = sysconf(_SC_PAGESIZE);
    CHECK(page >= 128 && page % 8 == 0);
    size_t ps = (size_t)page;
    uint8_t *src = map_pages(3 * ps), *dst = map_pages(17 * ps);
    CHECK(!mprotect(src + ps, ps, PROT_READ | PROT_WRITE));
    for (unsigned y = 0; y < 8; ++y)
        CHECK(!mprotect(dst + (2*y+1)*ps, ps, PROT_READ | PROT_WRITE));
    /* Every source/destination edge, forward/reverse row traversal, both
     * signs and zero, all corners, Put/Add. Read-only coefficients also catch
     * accidental writes to the block which ordinary IDCT is allowed to do. */
    for (int source_end = 0; source_end < 2; ++source_end)
        for (int dest_end = 0; dest_end < 2; ++dest_end)
            for (int reverse = 0; reverse < 2; ++reverse)
                for (int dc = -2048; dc <= 2040; dc += 8)
                    for (int corner = -1; corner <= 1; ++corner)
                        for (int add = 0; add <= 1; ++add) {
                            _Alignas(64) int16_t copy[64] = {0};
                            _Alignas(64) uint8_t expected[64];
                            int16_t *block = (int16_t *)(src + ps + (source_end ? ps-128 : 0));
                            ptrdiff_t stride = reverse ? -(ptrdiff_t)(2*ps) : (ptrdiff_t)(2*ps);
                            uint8_t *base = dst + (reverse ? 15 : 1)*ps + (dest_end ? ps-8 : 0);
                            case_dc = dc; case_corner = corner; case_add = add; case_stride = stride;
                            CHECK(!mprotect(src + ps, ps, PROT_READ | PROT_WRITE));
                            copy[0] = dc; copy[63] = corner;
                            memcpy(block, copy, 128);
                            CHECK(!mprotect(src + ps, ps, PROT_READ));
                            for (unsigned y = 0; y < 8; ++y) {
                                uint8_t *row_page = dst + (2*y+1)*ps;
                                memset(row_page, 0x95, ps);
                                /* Initialize logical rows after all canaries below. */
                            }
                            for (unsigned y = 0; y < 8; ++y)
                                for (unsigned x = 0; x < 8; ++x)
                                    expected[8*y+x] = base[y*stride+x] = (uint8_t)(x*19+y*37);
                            if (add) oracle_add(expected, 8, copy);
                            else oracle_put(expected, 8, copy);
                            CHECK(et_reconstruct_dc(base, stride, block, add) == 1);
                            for (unsigned y = 0; y < 8; ++y) {
                                CHECK(!memcmp(base + y*stride, expected + 8*y, 8));
                                uint8_t *row_page = dst + (2*y+1)*ps;
                                size_t lo = dest_end ? 0 : 8, hi = dest_end ? ps-8 : ps;
                                for (size_t i = lo; i < hi; ++i) CHECK(row_page[i] == 0x95);
                            }
                            CHECK(block[0] == dc && block[63] == corner);
                            for (unsigned k = 1; k < 63; ++k) CHECK(block[k] == 0);
                            ++accepted;
                        }
    /* Zero residual Add must not access destination at all. */
    _Alignas(64) int16_t zero[64] = {0};
    for (int corner = -1; corner <= 1; ++corner) {
        zero[63] = corner;
        CHECK(et_reconstruct_dc(dst, 128, zero, 1) == 1);
    }
    CHECK(!munmap(src, 3*ps));
    CHECK(!munmap(dst, 17*ps));
}

int main(int argc, char **argv)
{
    const uint64_t endian = 1;
    CHECK(*(const uint8_t *)&endian == 1); /* Header is explicitly LE-only. */
    if (argc == 2 && !strcmp(argv[1], "--refusals")) {
        refusals();
    } else {
        CHECK(argc == 1);
        raw_transform();
        exhaustive();
        printf("exhaustive oracle comparisons passed: %" PRIu64 "\n", accepted);
        fflush(stdout);
        refusals();
        guard_pages();
    }
    printf("PASS: accepted=%" PRIu64 " refused=%" PRIu64
           " Add-lane-model=%" PRIu64 " raw-IDCT=%" PRIu64
           " (native only; no ET execution)\n",
           accepted, refused, modeled, raw_idcts);
    return 0;
}
