/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "arena.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define SLOTS 96
static uint32_t next_u32(uint32_t *s) { *s=*s*1664525u+1013904223u; return *s; }
static void fill(uint8_t *p, size_t n, uint8_t tag) { memset(p,tag,n); }
static void check(const uint8_t *p, size_t n, uint8_t tag) { for(size_t i=0;i<n;i++)assert(p[i]==tag); }
int main(void) {
    static uint8_t store[65536] __attribute__((aligned(64)));
    ETAACFullArena a; uint8_t *x,*y,*z,*q,*grown;
    void *slot[SLOTS]={0}; size_t size[SLOTS]={0}; uint8_t tag[SLOTS]={0}; uint32_t seed=1;
    etaac_arena_init(&a,store,sizeof(store)); assert(a.bytes==sizeof(store));
    x=etaac_arena_alloc(&a,64,100); y=etaac_arena_alloc(&a,256,300); z=etaac_arena_alloc(&a,16,200);
    assert(x&&y&&z && !((uintptr_t)x&63) && !((uintptr_t)y&255)); fill(x,100,0xa5);
    etaac_arena_free(&a,y); q=etaac_arena_alloc(&a,128,240); assert(q && !((uintptr_t)q&127));
    grown=etaac_arena_realloc(&a,x,400); assert(grown); check(grown,100,0xa5);
    etaac_arena_free(&a,z); etaac_arena_free(&a,q); etaac_arena_free(&a,grown); assert(a.used==0);
    /* In-place growth must retain a split free tail for a subsequent large allocation. */
    x=etaac_arena_alloc(&a,16,128); y=etaac_arena_alloc(&a,16,4096); assert(x&&y); fill(x,128,0x5c);
    etaac_arena_free(&a,y); grown=etaac_arena_realloc(&a,x,512); assert(grown==x); check(grown,128,0x5c);
    z=etaac_arena_alloc(&a,16,32768); assert(z); etaac_arena_free(&a,z); etaac_arena_free(&a,grown); assert(a.used==0);
    assert(!etaac_arena_alloc(&a,3,4)); assert(a.failures);
    assert(!etaac_arena_alloc(&a,64,sizeof(store))); assert(a.failures>1);
    /* Deterministic allocation/realloc/free stress catches link/coalesce corruption. */
    for(unsigned step=0;step<30000;step++) {
        unsigned i=next_u32(&seed)%SLOTS, op=next_u32(&seed)%3;
        if(!slot[i] && op!=2) {
            size[i]=(next_u32(&seed)%1536)+1; tag[i]=(uint8_t)(i+step); slot[i]=etaac_arena_alloc(&a,1u<<(4+(next_u32(&seed)%5)),size[i]);
            if(slot[i]) fill(slot[i],size[i],tag[i]);
        } else if(slot[i] && op==0) {
            size_t want=(next_u32(&seed)%2048)+1; check(slot[i],size[i],tag[i]);
            void *n=etaac_arena_realloc(&a,slot[i],want);
            if(n) { check(n,size[i]<want?size[i]:want,tag[i]); slot[i]=n; size[i]=want; fill(slot[i],size[i],tag[i]); }
        } else if(slot[i]) { check(slot[i],size[i],tag[i]); etaac_arena_free(&a,slot[i]); slot[i]=0; size[i]=0; }
    }
    uint64_t span=etaac_arena_publish_bytes(&a);
    assert(span<=a.bytes && !(span&63));
    for(unsigned i=0;i<SLOTS;i++)if(slot[i])assert((uintptr_t)slot[i]+size[i]<=(uintptr_t)a.base+span);
    for(unsigned i=0;i<SLOTS;i++)if(slot[i]) { check(slot[i],size[i],tag[i]); etaac_arena_free(&a,slot[i]); }
    assert(a.used==0);
    assert(etaac_arena_publish_bytes(&a)==64);
    return 0;
}
