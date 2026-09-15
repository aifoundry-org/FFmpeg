/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef ET_KERNEL_IDCT_SIMD_H
#define ET_KERNEL_IDCT_SIMD_H

#include <stddef.h>
#include <stdint.h>

#ifndef ET_IDCT_ROWS
#define ET_IDCT_ROWS 0
#endif
#ifndef ET_IDCT_PACKED
#define ET_IDCT_PACKED 0
#endif

/* Optional eight-rows-in-parallel pass; wraps every output to int16_t. */
void et_idct_simd_rows(int16_t *block);

/* block is the int16_t result of the exact scalar or SIMD row pass. It is
 * not modified. add=0 implements Put, add=1 implements Add. No alignment
 * beyond int16_t alignment is required here; the scalar row template needs 8.
 * PACKED proves stronger alignment at runtime and otherwise falls back. */
void et_idct_simd_columns(uint8_t *dest, ptrdiff_t stride,
                          const int16_t *block, int add);

#endif
