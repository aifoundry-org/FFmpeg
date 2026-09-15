/* SPDX-License-Identifier: LGPL-2.1-or-later
 * ET scalar accesses must not trap on unaligned C-library arguments. Volatile
 * byte accesses prevent the compiler lowering these loops back to builtins. */
#include <stddef.h>
__attribute__((used, externally_visible)) void *memcpy(void *restrict dest, const void *restrict src, size_t n)
{
    volatile unsigned char *d = dest;
    const volatile unsigned char *s = src;
    for (size_t i=0; i<n; i++) d[i] = s[i];
    return dest;
}
__attribute__((used, externally_visible)) void *memset(void *dest, int value, size_t n)
{
    volatile unsigned char *d = dest;
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
