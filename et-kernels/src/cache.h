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

/* Read-only L2 hint for an already validated prediction rectangle. Plane
 * strides and bounds guarantee these COMPLETE lines belong to the reference
 * allocation, including the optional halo. Never alter cache partitioning. */
static inline void et_prefetch_rectangle(const uint8_t *src, size_t stride,
                                         unsigned bytes, unsigned rows)
{
#ifdef ET_DEVICE
    uintptr_t base = (uintptr_t)src & ~(uintptr_t)63;
    unsigned columns = (((uintptr_t)src & 63) + bytes + 63) / 64;
    for (unsigned col=0; col<columns; col++) {
        for (unsigned row=0; row<rows; row+=16) {
            unsigned n = rows-row;
            if (n > 16) n = 16;
            uint64_t value = (UINT64_C(1) << 58) |
                ((base + col*64 + row*stride) & UINT64_C(0xFFFFFFFFFFC0)) | (n-1);
            __asm__ volatile("mv x31, %1\n\tcsrw 0x81f, %0"
                :: "r"(value), "r"(stride) : "x31", "memory");
        }
    }
#else
    (void)src; (void)stride; (void)bytes; (void)rows;
#endif
}
#endif
