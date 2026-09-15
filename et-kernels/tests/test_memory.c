/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Compile the actual freestanding memory routines under distinct host names. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define memcpy et_test_memcpy
#define memset et_test_memset
#define memmove et_test_memmove
#define memcmp et_test_memcmp
#include "../src/libc.c"
#undef memcpy
#undef memset
#undef memmove
#undef memcmp

int main(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE), tests = 0;
    uint8_t *a = mmap(NULL, page*3, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    uint8_t *b = mmap(NULL, page*3, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    assert(a != MAP_FAILED && b != MAP_FAILED);
    assert(!mprotect(a+page, page, PROT_READ|PROT_WRITE));
    assert(!mprotect(b+page, page, PROT_READ|PROT_WRITE));
    for (size_t i=0; i<page; i++) a[page+i] = (uint8_t)(i*79+3);
    for (size_t n=0; n<=2048; n++) {
        for (unsigned misalign=0; misalign<16; misalign++) {
            /* End immediately at a protected page. Destination misalignment
             * differs from source; prefix/suffix canaries are checked fully. */
            uint8_t *s = a+2*page-n;
            uint8_t *d = b+2*page-n-misalign;
            memset(b+page, 0xa5, page);
            assert(et_test_memcpy(d, s, n) == d);
            assert(!memcmp(d, s, n));
            for (uint8_t *p=b+page; p<d; p++) assert(*p == 0xa5);
            for (uint8_t *p=d+n; p<b+2*page; p++) assert(*p == 0xa5);
            int value = (int)(n*13)-1024;
            assert(et_test_memset(d, value, n) == d);
            for (size_t i=0; i<n; i++) assert(d[i] == (uint8_t)value);
            for (uint8_t *p=b+page; p<d; p++) assert(*p == 0xa5);
            for (uint8_t *p=d+n; p<b+2*page; p++) assert(*p == 0xa5);
            tests++;
        }
    }
    munmap(a, page*3); munmap(b, page*3);
    printf("PASS: %zu bounded copy/fill cases, all alignments, guard pages, canaries\n", tests);
    return 0;
}
