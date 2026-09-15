/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef ET_RECONSTRUCT_H
#define ET_RECONSTRUCT_H
#include <stdint.h>
#include <stddef.h>
#ifndef ET_FAST_DC
#define ET_FAST_DC 0
#endif
typedef uint64_t et_dc_u64 __attribute__((may_alias));

/* Exact common MPEG2 case, including the mandatory mismatch-control corner.
 * DC must be an integer pixel value with no int16 row overflow. An isolated
 * corner coefficient -1..1 contributes less than 0.27 to any final sample;
 * DC row/column constant error is <0.016 for this range. Together they cannot
 * cross the nearest-integer boundary. No approximation/tolerance is involved.
 * The general IDCT remains mandatory for fractional-pixel DC or any other AC.
 * Unlike FFmpeg's IDCT API this helper leaves the disposable block unchanged. */
static inline int et_reconstruct_dc(uint8_t *dst, ptrdiff_t stride,
                                    const int16_t *block, int add)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
    return 0;
#endif
    if (((uintptr_t)dst | (uintptr_t)stride | (uintptr_t)block) & 7) return 0;
    int dc = block[0];
    if ((dc & 7) || dc < -2048 || dc > 2040) return 0;
    const et_dc_u64 *words = (const et_dc_u64 *)block;
    if ((words[0] & UINT64_C(0xffffffffffff0000)) ||
        (words[15] & UINT64_C(0x0000ffffffffffff)) ||
        block[63] < -1 || block[63] > 1) return 0;
    for (unsigned i=1; i<15; i++) if (words[i]) return 0;
    dc /= 8;
    if (!add) {
        unsigned byte = dc < 0 ? 0 : dc > 255 ? 255 : (unsigned)dc;
        uint64_t word = byte * UINT64_C(0x0101010101010101);
        for (unsigned y=0; y<8; y++) {
            *(et_dc_u64 *)dst = word;
            if (y != 7) dst += stride;
        }
    } else if (dc) {
#ifdef ET_DEVICE
        /* Eight unsigned predictions, exact signed residual add and clamp.
         * The alignment test proves each FG32/FSC32 interval fits one window. */
        uintptr_t masks, rows = 8;
        __asm__ volatile(
            "mova.x.m %[mask]\n"
            "mov.m.x m0, zero, 255\n"
            "fbcx.ps f1, %[dc]\n"
            "1:\n"
            "fg32b.ps f0, %[bconf](%[dst])\n"
            "fandi.pi f0, f0, 255\n"
            "fadd.pi f0, f0, f1\n"
            "fsatu8.pi f0, f0\n"
            "fpackrepb.pi f0, f0\n"
            "mov.m.x m0, zero, 3\n"
            "fsc32w.ps f0, %[wconf](%[dst])\n"
            "mov.m.x m0, zero, 255\n"
            "addi %[rows], %[rows], -1\n"
            "beqz %[rows], 2f\n"
            "add %[dst], %[dst], %[stride]\n"
            "j 1b\n"
            "2:\n"
            "mova.m.x %[mask]\n"
            : [dst] "+&r"(dst), [rows] "+&r"(rows), [mask] "=&r"(masks)
            : [stride] "r"(stride), [dc] "r"(dc),
              [bconf] "r"(UINT64_C(0x398a418820)), [wconf] "r"(8ul)
            : "f0", "f1", "memory");
#else
        for (unsigned y=0; y<8; y++) {
            for (unsigned x=0; x<8; x++) {
                int v = dst[x] + dc;
                dst[x] = v < 0 ? 0 : v > 255 ? 255 : v;
            }
            if (y != 7) dst += stride;
        }
#endif
    }
    return 1;
}
#endif
