/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Compile FFmpeg's scalar template unchanged, identity coefficient ordering.
 * ET_IDCT_SIMD=1 selects exact ET integer SIMD columns for Put/Add only.
 * ET_IDCT_ROWS=1 additionally selects SIMD rows, ET_IDCT_PACKED=1 fast I/O.
 * All three default to zero; ET_IDCT_SIMD=0 is the unchanged scalar control.
 * Compile src/idct_simd.c alongside this file when selecting SIMD. */
#ifndef ET_IDCT_SIMD
#define ET_IDCT_SIMD 0
#endif
#if ET_IDCT_SIMD
/* Retain the unchanged scalar entry points as test oracles.  The ordinary
 * non-Put/Add IDCT remains scalar; MPEG-2 decoding uses Put and Add. */
#define ff_simple_idct_put_int16_8bit et_simple_idct_put_scalar
#define ff_simple_idct_add_int16_8bit et_simple_idct_add_scalar
#endif
#include "config.h"
#include "libavutil/intreadwrite.h"
#include "libavcodec/mathops.h"
#define IN_IDCT_DEPTH 16
#define BIT_DEPTH 8
#include "libavcodec/simple_idct_template.c"

#if ET_IDCT_SIMD
#undef ff_simple_idct_put_int16_8bit
#undef ff_simple_idct_add_int16_8bit
#include "idct_simd.h"

void ff_simple_idct_put_int16_8bit(uint8_t *dest, ptrdiff_t stride, int16_t *block)
{
#if ET_IDCT_ROWS
    et_idct_simd_rows(block);
#else
    for (int i = 0; i < 8; ++i)
        idctRowCondDC_int16_8bit(block + i * 8, 0);
#endif
    et_idct_simd_columns(dest, stride, block, 0);
}

void ff_simple_idct_add_int16_8bit(uint8_t *dest, ptrdiff_t stride, int16_t *block)
{
#if ET_IDCT_ROWS
    et_idct_simd_rows(block);
#else
    for (int i = 0; i < 8; ++i)
        idctRowCondDC_int16_8bit(block + i * 8, 0);
#endif
    et_idct_simd_columns(dest, stride, block, 1);
}
#endif
