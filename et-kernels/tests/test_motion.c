/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Standalone native test: see MOTION.md. Never launches an ET device/emulator. */
#define _GNU_SOURCE
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "../src/motion.h"

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#define POISON(p,n) __asan_poison_memory_region(p,n)
#define UNPOISON(p,n) __asan_unpoison_memory_region(p,n)
#else
#define POISON(p,n) ((void)0)
#define UNPOISON(p,n) ((void)0)
#endif

static uint64_t state = UINT64_C(0x61786d7065673201);
static uint64_t random64(void)
{
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * UINT64_C(2685821657736338717);
}

static void oracle(uint8_t *d, const uint8_t *s, size_t stride,
                   unsigned n, unsigned hx, unsigned hy, unsigned avg)
{
    for (unsigned y = 0; y < n; ++y)
        for (unsigned x = 0; x < n; ++x) {
            unsigned sum = 0, samples = (hx+1)*(hy+1);
            for (unsigned j = 0; j <= hy; ++j)
                for (unsigned i = 0; i <= hx; ++i)
                    sum += s[(y+j)*stride+x+i];
            unsigned pred = (sum + samples/2)/samples;
            size_t pos = y*stride+x;
            d[pos] = (uint8_t)(avg ? (d[pos]+pred+1)/2 : pred);
        }
}

static uint64_t xy_oracle(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    uint64_t out = 0;
    for (unsigned i = 0; i < 64; i += 8) {
        unsigned sum = ((a >> i)&255) + ((b >> i)&255) +
                       ((c >> i)&255) + ((d >> i)&255);
        out |= (uint64_t)((sum+2)/4) << i;
    }
    return out;
}

static void arithmetic_tests(void)
{
    const uint64_t ones = UINT64_C(0x0101010101010101);
    for (unsigned a = 0; a < 256; ++a)
        for (unsigned b = 0; b < 256; ++b) {
            assert(et_mc_avg8(a*ones,b*ones) == ((a+b+1)/2)*ones);
            uint64_t aa = (a*ones) ^ UINT64_C(0xff00ff00ff00ff00);
            uint64_t bb = (b*ones) ^ UINT64_C(0x00ff00ff00ff00ff);
            uint64_t result = et_mc_avg8(aa,bb);
            for (unsigned shift = 0; shift < 64; shift += 8)
                assert(((result >> shift)&255) ==
                       ((((aa >> shift)&255)+((bb >> shift)&255)+1)/2));
        }
    static const uint8_t edge[] = {0,1,2,3,4,5,63,64,65,126,127,128,129,250,251,252,253,254,255};
    for (size_t i = 0; i < sizeof(edge); ++i)
        for (size_t j = 0; j < sizeof(edge); ++j)
            for (size_t k = 0; k < sizeof(edge); ++k)
                for (size_t l = 0; l < sizeof(edge); ++l) {
                    uint64_t a = edge[i]*ones ^ UINT64_C(0xff00ff00ff00ff00);
                    uint64_t b = edge[j]*ones ^ UINT64_C(0x00ff00ff00ff00ff);
                    uint64_t c = edge[k]*ones ^ UINT64_C(0x0000ffff0000ffff);
                    uint64_t d = edge[l]*ones ^ UINT64_C(0xffff0000ffff0000);
                    uint64_t expected = xy_oracle(a,b,c,d);
                    assert(et_mc_xy8_evenodd(a,b,c,d) == expected);
                    assert(et_mc_xy8_parts(a,b,c,d) == expected);
                }
    for (unsigned i = 0; i < 250000; ++i) {
        uint64_t a=random64(), b=random64(), c=random64(), d=random64();
        uint64_t expected = xy_oracle(a,b,c,d);
        assert(et_mc_xy8_evenodd(a,b,c,d) == expected);
        assert(et_mc_xy8_parts(a,b,c,d) == expected);
    }
    /* Nested rounded pair averages are NOT xy2. */
    assert(et_mc_xy8(0,0,0,ones) == 0);
    assert(et_mc_avg8(et_mc_avg8(0,0),et_mc_avg8(0,ones)) == ones);
}

static void fill(uint8_t *p, size_t bytes, unsigned pattern)
{
    for (size_t i = 0; i < bytes; ++i)
        switch (pattern) {
        case 0: p[i] = 0; break;
        case 1: p[i] = 255; break;
        case 2: p[i] = (i&1) ? 255 : 0; break;
        case 3: p[i] = (i&1) ? 0 : 255; break;
        case 4: p[i] = (uint8_t)i; break;
        case 5: p[i] = (uint8_t)(255-i); break;
        default: p[i] = (uint8_t)random64(); break;
        }
}

static void block_tests(void)
{
    enum { CAP = 4096 };
    _Alignas(64) uint8_t source[CAP], before[CAP], actual[CAP], expected[CAP];
    unsigned cases = 0;
    for (unsigned iter = 0; iter < 24000; ++iter) {
        unsigned n = (iter&1) ? 8 : 16;
        unsigned mode = (iter/2)&7, hx=mode&1, hy=(mode>>1)&1, avg=mode>>2;
        unsigned offset = (iter/16)&63; /* Include 32/64-byte boundary crossings. */
        size_t stride = 64*(1+(iter/128)%3);
        size_t sp = 64+offset, dp = 64+8*((iter/384)&7);
        fill(source,CAP,iter/1024);
        memcpy(before,source,CAP);
        fill(actual,CAP,6);
        memcpy(expected,actual,CAP);
        oracle(expected+dp,source+sp,stride,n,hx,hy,avg);
        /* With ASan, poison every byte except the exact reference rectangle.
         * Hardware page guards below complement ASan's 8-byte granularity. */
        POISON(source,CAP);
        for (unsigned y=0; y<n+hy; ++y) UNPOISON(source+sp+y*stride,n+hx);
        et_mc_predict(actual+dp,source+sp,stride,n,hx,hy,avg);
        UNPOISON(source,CAP);
        assert(!memcmp(actual,expected,CAP));
        assert(!memcmp(source,before,CAP));
        ++cases;
    }
    printf("blocks: %u cases, all modes/source alignments/sizes, canaries OK\n",cases);
}

static void guard_tests(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE), stride = 2*page;
    assert(page >= 64 && !(page&63));
    /* Every row has a guard on BOTH sides. Source rows are read-only. */
    for (unsigned n=8; n<=16; n+=8)
        for (unsigned mode=0; mode<8; ++mode)
            for (unsigned right=0; right<2; ++right) {
                unsigned hx=mode&1, hy=(mode>>1)&1, avg=mode>>2;
                size_t bytes = (2*(n+hy)+1)*page;
                uint8_t *s=mmap(NULL,bytes,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
                uint8_t *d=mmap(NULL,bytes,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
                uint8_t *expected=malloc(bytes);
                assert(s!=MAP_FAILED && d!=MAP_FAILED && expected);
                for (unsigned y=0; y<n+hy; ++y) {
                    assert(!mprotect(s+page+y*stride,page,PROT_READ|PROT_WRITE));
                    fill(s+page+y*stride,page,6);
                    assert(!mprotect(s+page+y*stride,page,PROT_READ));
                }
                for (unsigned y=0; y<n; ++y) {
                    assert(!mprotect(d+page+y*stride,page,PROT_READ|PROT_WRITE));
                    fill(d+page+y*stride,page,6);
                    memcpy(expected+page+y*stride,d+page+y*stride,page);
                }
                size_t sp=page+(right ? page-n-hx : 0);
                size_t dp=page+(right ? page-n : 0);
                oracle(expected+dp,s+sp,stride,n,hx,hy,avg);
                et_mc_predict(d+dp,s+sp,stride,n,hx,hy,avg);
                for (unsigned y=0; y<n; ++y)
                    assert(!memcmp(d+page+y*stride,expected+page+y*stride,page));
                assert(!munmap(s,bytes) && !munmap(d,bytes));
                free(expected);
            }
    /* Exact eight-byte arbitrary-alignment helper: ASan catches suffix
     * overreads at all alignments, including common two-aligned-ld tricks. */
    for (unsigned off=0; off<8; ++off) {
        _Alignas(64) uint8_t buf[64];
        fill(buf,sizeof(buf),6);
        uint64_t expect=et_mc_load8_bytes(buf+off);
        POISON(buf,sizeof(buf)); UNPOISON(buf+off,8);
        assert(et_mc_load8(buf+off)==expect);
        UNPOISON(buf,sizeof(buf));
    }
    puts("guards: exact row starts/ends, read-only source and byte-load bounds OK");
}

static void benchmark(void)
{
    _Alignas(64) uint8_t src[64*32], dst[64*32];
    fill(src,sizeof(src),6); fill(dst,sizeof(dst),6);
    struct timespec begin,end;
    uint64_t checksum=0;
    for (unsigned mode=0; mode<8; ++mode) {
        clock_gettime(CLOCK_MONOTONIC,&begin);
        for (unsigned i=0; i<100000; ++i) {
            et_mc_predict(dst,src+(i&7),64,16,mode&1,(mode>>1)&1,mode>>2);
            checksum += dst[(i>>3)&15];
        }
        clock_gettime(CLOCK_MONOTONIC,&end);
        double ns=(end.tv_sec-begin.tv_sec)*1e9+end.tv_nsec-begin.tv_nsec;
        printf("native-only mode %u: %.1f ns/16x16\n",mode,ns/100000);
    }
    printf("benchmark checksum: %" PRIu64 "\n",checksum);
}

int main(int argc, char **argv)
{
    arithmetic_tests(); block_tests(); guard_tests();
    if (argc>1 && !strcmp(argv[1],"--bench")) benchmark();
    printf("PASS impl=%d load=%d xy=%d (native, not ET silicon)\n",
           ET_MC_IMPL,ET_MC_LOAD,ET_MC_XY);
    return 0;
}
