/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Native exactness test: independent unchanged FFmpeg template vs the shared
 * SIMD instruction schedule.  See idct_simd.md for commands and limitations. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
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
#include "../src/idct_simd.h"

void ff_simple_idct_put_int16_8bit(uint8_t *, ptrdiff_t, int16_t *);
void ff_simple_idct_add_int16_8bit(uint8_t *, ptrdiff_t, int16_t *);

static uint32_t rng = 0x934acd71;
static uint64_t cases;
static uint64_t packed_eligible;
static uint32_t random32(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static int16_t s16(uint32_t n)
{
    n &= 65535;
    return n < 32768 ? (int)n : (int)n - 65536;
}

static void fail(const char *kind, unsigned index, int add)
{
    fprintf(stderr, "IDCT mismatch: %s case=%" PRIu64 " index=%u add=%d rng=%08x\n",
            kind, cases, index, add, rng);
    exit(1);
}

static void check(const int16_t input[64], int columns_only)
{
    /* All allowed row-template alignments modulo 32, including fast/fallback. */
    _Alignas(64) int16_t block_a[96], block_b[96];
    _Alignas(64) uint8_t output_a[1024], output_b[1024];
    unsigned block_offset = (cases & 3) * 4;
    ptrdiff_t stride = 8 + (random32() % 65);
    unsigned offset = 32 + (random32() & 15);
    /* Deterministically cover both halves of FG32 windows and both signs. */
    if ((cases & 7) == 0 || (cases & 7) == 2) {
        stride = 64;
        offset = (cases & 7) == 0 ? 32 : 40;
    }
    if ((cases & 255) == 64) stride = 0;
    if (random32() & 1) {
        offset += (unsigned)stride * 7;
        stride = -stride;
    }
    if (stride && !((uintptr_t)(block_b + block_offset) & 15) &&
        !(((uintptr_t)(output_b + offset) | (uintptr_t)stride) & 7))
        ++packed_eligible;
    for (int add = 0; add <= 1; ++add) {
        memset(block_a, 0xa5, sizeof(block_a));
        memcpy(block_a + block_offset, input, 128);
        memcpy(block_b, block_a, sizeof(block_a));
        for (unsigned i = 0; i < sizeof(output_a); ++i)
            output_a[i] = (uint8_t)random32();
        memcpy(output_b, output_a, sizeof(output_a));
        if (columns_only) {
            for (unsigned x = 0; x < 8; ++x) {
                if (add)
                    idctSparseColAdd_int16_8bit(output_a + offset + x, stride, block_a + block_offset + x);
                else
                    idctSparseColPut_int16_8bit(output_a + offset + x, stride, block_a + block_offset + x);
            }
            et_idct_simd_columns(output_b + offset, stride, block_b + block_offset, add);
        } else if (add) {
            oracle_add(output_a + offset, stride, block_a + block_offset);
            ff_simple_idct_add_int16_8bit(output_b + offset, stride, block_b + block_offset);
        } else {
            oracle_put(output_a + offset, stride, block_a + block_offset);
            ff_simple_idct_put_int16_8bit(output_b + offset, stride, block_b + block_offset);
        }
        for (unsigned i = 0; i < sizeof(output_a); ++i)
            if (output_a[i] != output_b[i]) fail(columns_only ? "columns/output" : "full/output", i, add);
        for (unsigned i = 0; i < 96; ++i)
            if (block_a[i] != block_b[i]) fail(columns_only ? "columns/block" : "full/block", i, add);
    }
    ++cases;
}

int main(int argc, char **argv)
{
    const unsigned random_cases = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 100000;
    _Alignas(8) int16_t block[64] = {0};
    static const int values[] = {-32768, -32767, -16384, -8192, -4096, -2048,
        -1024, -255, -1, 1, 255, 1024, 2047, 4095, 8191, 16383, 32766, 32767};

    check(block, 0);
    /* Exhaust every DC value, including wrapping of the row[0] * 8 shortcut.
     * Different DCs in each row exercise the fast path independently. */
    for (unsigned dc = 0; dc < 65536; ++dc) {
        memset(block, 0, sizeof(block));
        for (unsigned y = 0; y < 8; ++y)
            block[y * 8] = s16(dc + y * 7919);
        check(block, 0);
        /* Independent post-row DC sweep exercises column rounding and both
         * clipping boundaries without other frequencies hiding the result. */
        memset(block, 0, sizeof(block));
        for (unsigned x = 0; x < 8; ++x)
            block[x] = s16(dc + x * 7919);
        check(block, 1);
    }
    for (unsigned pos = 0; pos < 64; ++pos) {
        for (unsigned v = 0; v < sizeof(values) / sizeof(values[0]); ++v) {
            memset(block, 0, sizeof(block));
            block[pos] = values[v];
            check(block, 0);
            check(block, 1);
        }
    }
    for (unsigned p = 0; p < 256; ++p) {
        for (unsigned i = 0; i < 64; ++i)
            block[i] = (p & (1 << (i % 8))) ? INT16_MIN : INT16_MAX;
        check(block, 0);
        check(block, 1);
    }
    /* Every per-row DC/non-DC lane-mask pattern, not just all lanes together. */
    for (unsigned mask = 0; mask < 256; ++mask) {
        memset(block, 0, sizeof(block));
        for (unsigned row = 0; row < 8; ++row) {
            block[row * 8] = s16(mask * 271 + row * 7919);
            if (mask & (1u << row))
                block[row * 8 + 1 + ((row + mask) % 7)] = (row & 1) ? -1 : 32767;
        }
        check(block, 0);
    }
    for (unsigned n = 0; n < random_cases; ++n) {
        for (unsigned i = 0; i < 64; ++i) {
            uint32_t value = random32();
            /* Full int16 range, MPEG2-ish magnitudes, sparse, and low-half rows. */
            switch (n % 4) {
            case 0: block[i] = s16(value); break;
            case 1: block[i] = (int)(value % 4096) - 2048; break;
            case 2: block[i] = (value & 7) ? 0 : s16(value >> 3); break;
            default: block[i] = (i % 8 >= 4) ? 0 : s16(value); break;
            }
        }
        check(block, 0);
        check(block, 1);
    }
    printf("PASS: %" PRIu64 " block cases, Put+Add each; all int16 DCs, sparse/extreme, "
           "%u random full+column-only cases; destination/coeff canaries and negative strides\n",
           cases, random_cases);
    printf("Variants ROWS=%d PACKED=%d; fast-I/O eligible=%" PRIu64
           " fallback=%" PRIu64 " (both signs of stride, alignments 0/8/16/24)\n",
           ET_IDCT_ROWS, ET_IDCT_PACKED, packed_eligible, cases - packed_eligible);
    if (!packed_eligible || packed_eligible == cases) return 1;
    return 0;
}
