/* SPDX-License-Identifier: LGPL-2.1-or-later
 * ET scalar accesses must not trap on unaligned C-library arguments. Volatile
 * byte accesses prevent the compiler lowering these loops back to builtins. */
#include <stddef.h>
#include <stdint.h>
#ifndef ET_WIDE_MEM
#define ET_WIDE_MEM 1
#endif
typedef uint64_t et_alias_u64 __attribute__((may_alias));
__attribute__((used, externally_visible)) void *memcpy(void *restrict dest, const void *restrict src, size_t n)
{
    volatile unsigned char *d = dest;
    const volatile unsigned char *s = src;
#if ET_WIDE_MEM
    if (!(((uintptr_t)d ^ (uintptr_t)s) & 7)) {
        while (n && ((uintptr_t)d & 7)) { *d++ = *s++; n--; }
        while (n >= 32) {
            volatile et_alias_u64 *dw = (volatile et_alias_u64 *)d;
            const volatile et_alias_u64 *sw = (const volatile et_alias_u64 *)s;
            uint64_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
            dw[0] = a; dw[1] = b; dw[2] = c; dw[3] = e;
            d += 32; s += 32; n -= 32;
        }
        while (n >= 8) {
            *(volatile et_alias_u64 *)d = *(const volatile et_alias_u64 *)s;
            d += 8; s += 8; n -= 8;
        }
    }
#endif
    for (size_t i=0; i<n; i++) d[i] = s[i];
    return dest;
}
__attribute__((used, externally_visible)) void *memset(void *dest, int value, size_t n)
{
    volatile unsigned char *d = dest;
#if ET_WIDE_MEM
    uint64_t word = (unsigned char)value * UINT64_C(0x0101010101010101);
    while (n && ((uintptr_t)d & 7)) { *d++ = (unsigned char)value; n--; }
    while (n >= 32) {
        volatile et_alias_u64 *dw = (volatile et_alias_u64 *)d;
        dw[0] = word; dw[1] = word; dw[2] = word; dw[3] = word;
        d += 32; n -= 32;
    }
    while (n >= 8) { *(volatile et_alias_u64 *)d = word; d += 8; n -= 8; }
#endif
    for (size_t i=0; i<n; i++) d[i] = (unsigned char)value;
    return dest;
}
__attribute__((used, externally_visible)) void *memmove(void *dest, const void *src, size_t n)
{
    volatile unsigned char *d = dest;
    const volatile unsigned char *s = src;
    if ((size_t)d < (size_t)s) for (size_t i=0; i<n; i++) d[i] = s[i];
    else while (n) { n--; d[n] = s[n]; }
    return dest;
}
__attribute__((used, externally_visible)) int memcmp(const void *a, const void *b, size_t n)
{
    const volatile unsigned char *x=a, *y=b;
    for (size_t i=0; i<n; i++) if (x[i] != y[i]) return x[i]-y[i];
    return 0;
}
