/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef ET_KERNEL_CACHE_H
#define ET_KERNEL_CACHE_H
#include <stdint.h>
#include <stddef.h>
static inline uint64_t et_cycles(void)
{
#ifdef ET_DEVICE
    uint64_t value;
    /* RTLMIN-6496: four aligned reads. rdcycle traps in user mode. */
    __asm__ volatile(".p2align 4\n\tcsrr %0, hpmcounter3\n\t"
                     "csrr %0, hpmcounter3\n\tcsrr %0, hpmcounter3\n\t"
                     "csrr %0, hpmcounter3" : "=r"(value) :: "memory");
    return value;
#else
    return 0;
#endif
}
/* Explicit EvictVA to L3/DRAM, never the implicit runtime/uber flush.
 * CSR encoding from et-platform etsoc/isa/cacheops-umode.h. Every command
 * covers at most 16 cache lines; wait before reusing the issue registers. */
static inline void et_evict(const void *ptr, size_t bytes)
{
#ifdef ET_DEVICE
    uintptr_t first = (uintptr_t)ptr & ~(uintptr_t)63;
    uintptr_t last = ((uintptr_t)ptr + bytes + 63) & ~(uintptr_t)63;
    __asm__ volatile("fence" ::: "memory");
    while (first < last) {
        size_t n = (last - first) / 64;
        if (n > 16) n = 16;
        uint64_t val = (UINT64_C(2) << 58) | (first & UINT64_C(0xFFFFFFFFFFC0)) | (n-1);
        __asm__ volatile("li x31, 64\n\tcsrw 0x89f, %0\n\tcsrwi 0x830, 6\n\tfence"
                         :: "r"(val) : "x31", "memory");
        first += n * 64;
    }
#else
    (void)ptr; (void)bytes;
#endif
}
#endif
