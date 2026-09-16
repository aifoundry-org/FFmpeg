/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "arena.h"
#include <stdint.h>
#include <string.h>
#define ARENA_COOKIE UINT32_C(0xaac4a11c)
#define MIN_PAYLOAD 16u
typedef struct Block Block;
struct Block { uint64_t bytes; Block *prev, *next; uint32_t used, cookie; };
#define HSIZE ((sizeof(Block)+15u)&~(size_t)15u)

static uint64_t round_up(uint64_t n, uint64_t a)
{ return n > UINT64_MAX-(a-1) ? 0 : (n+a-1)&~(a-1); }
static int aligned(size_t n) { return n && !(n&(n-1)) && n<=4096; }
static int arena_ok(const ETAACFullArena *a)
{
    uintptr_t lo, hi;
    if (!a || a->cookie!=ARENA_COOKIE || !a->base || !a->first || !a->bytes) return 0;
    lo=(uintptr_t)a->base; hi=lo+a->bytes;
    return hi>=lo && !(lo&63) && !(a->bytes&63);
}
static int block_basic(const ETAACFullArena *a, const Block *b)
{
    uintptr_t lo, hi, p, payload, end;
    if (!arena_ok(a) || !b) return 0;
    lo=(uintptr_t)a->base; hi=lo+a->bytes; p=(uintptr_t)b;
    if (p<lo || p>hi-HSIZE || (p&15) || b->cookie!=ARENA_COOKIE || b->used>1) return 0;
    payload=p+HSIZE;
    if (b->bytes>hi-payload) return 0;
    end=payload+b->bytes;
    return end>=payload && end<=hi;
}
/* List topology is intentionally contiguous.  Validate every pointer before
 * following or writing it; corrupted allocator metadata is a hard failure. */
static int block_ok(const ETAACFullArena *a, const Block *b)
{
    uintptr_t end;
    if (!block_basic(a,b)) return 0;
    end=(uintptr_t)b+HSIZE+b->bytes;
    if (b->prev) {
        if (!block_basic(a,b->prev) || b->prev->next!=b ||
            (uintptr_t)b->prev+HSIZE+b->prev->bytes!=(uintptr_t)b) return 0;
    } else if (b!=(const Block *)a->first) return 0;
    if (b->next) {
        if ((uintptr_t)b->next!=end || !block_basic(a,b->next) || b->next->prev!=b) return 0;
    } else if (end!=(uintptr_t)a->base+a->bytes) return 0;
    return 1;
}
static void fail(ETAACFullArena *a) { if(a) a->failures++; }
void etaac_arena_init(ETAACFullArena *a, void *base, uint64_t bytes)
{
    Block *b;
    if (!a) return;
    memset(a,0,sizeof(*a));
    if (!base || ((uintptr_t)base&63) || bytes < HSIZE+MIN_PAYLOAD) return;
    bytes &= ~UINT64_C(63);
    a->base=base; a->bytes=bytes; a->cookie=ARENA_COOKIE;
    b=(Block *)base; b->bytes=bytes-HSIZE; b->prev=0; b->next=0; b->used=0; b->cookie=ARENA_COOKIE; a->first=b;
}
static Block *header(const ETAACFullArena *a, void *ptr)
{
    uintptr_t p, lo;
    Block *b;
    if (!arena_ok(a) || !ptr) return 0;
    p=(uintptr_t)ptr; lo=(uintptr_t)a->base;
    if (p<lo+HSIZE || p>(uintptr_t)a->base+a->bytes) return 0;
    b=(Block *)(p-HSIZE);
    return block_ok(a,b) && b->used ? b : 0;
}
static void link_after(Block *left, Block *right)
{
    right->prev=left; right->next=left->next;
    if (right->next) right->next->prev=right;
    left->next=right;
}
void *etaac_arena_alloc(ETAACFullArena *a, size_t alignment, size_t bytes)
{
    Block *b;
    uint64_t need;
    if (!arena_ok(a) || !aligned(alignment) || !bytes) { fail(a); return 0; }
    need=round_up(bytes,16); if(!need) { fail(a); return 0; }
    for (b=(Block *)a->first; b;) {
        uintptr_t payload,q; uint64_t avail,tail;
        if (!block_ok(a,b)) { fail(a); return 0; }
        if (b->used) { b=b->next; continue; }
        payload=(uintptr_t)b+HSIZE;
        /* Keep a real nonempty leading free block when a new header is needed
         * immediately before an aligned return pointer. */
        q=round_up(payload+HSIZE+MIN_PAYLOAD,alignment);
        if (!q || q<payload || q-HSIZE<payload) { b=b->next; continue; }
        if (q-HSIZE==(uintptr_t)b) q=payload; /* direct fit (normally impossible with the bias) */
        if (q!=payload) {
            Block *allocated=(Block *)(q-HSIZE); uint64_t lead=(uintptr_t)allocated-payload;
            if (lead<MIN_PAYLOAD || b->bytes<lead+HSIZE || !block_ok(a,b)) { b=b->next; continue; }
            avail=b->bytes-lead-HSIZE;
            if (avail<need) { b=b->next; continue; }
            allocated->bytes=avail; allocated->used=0; allocated->cookie=ARENA_COOKIE;
            link_after(b,allocated); b->bytes=lead; b=allocated;
        }
        if (!block_ok(a,b) || b->bytes<need) { fail(a); return 0; }
        tail=b->bytes-need;
        if (tail>=HSIZE+MIN_PAYLOAD) {
            Block *right=(Block *)((uintptr_t)b+HSIZE+need);
            right->bytes=tail-HSIZE; right->used=0; right->cookie=ARENA_COOKIE;
            link_after(b,right); b->bytes=need;
        }
        b->used=1; a->used+=b->bytes; if(a->used>a->peak)a->peak=a->used;
        return (void *)((uintptr_t)b+HSIZE);
    }
    fail(a); return 0;
}
static void merge_next(Block *b)
{
    Block *n=b->next;
    b->bytes+=HSIZE+n->bytes; b->next=n->next;
    if(b->next)b->next->prev=b;
}
void etaac_arena_free(ETAACFullArena *a, void *ptr)
{
    Block *b;
    if(!ptr)return;
    b=header(a,ptr); if(!b){fail(a);return;}
    b->used=0; a->used-=b->bytes;
    if(b->next) { if(!block_ok(a,b->next)){fail(a);return;} if(!b->next->used)merge_next(b); }
    if(b->prev) { if(!block_ok(a,b->prev)){fail(a);return;} if(!b->prev->used)merge_next(b->prev); }
}
void *etaac_arena_realloc(ETAACFullArena *a, void *ptr, size_t bytes)
{
    Block *b; uint64_t need,old,available,tail; void *q;
    if(!ptr)return etaac_arena_alloc(a,16,bytes);
    if(!bytes){etaac_arena_free(a,ptr);return 0;}
    b=header(a,ptr); if(!b){fail(a);return 0;}
    need=round_up(bytes,16); if(!need){fail(a);return 0;}
    old=b->bytes;
    if(need<=old) {
        tail=old-need;
        if(tail>=HSIZE+MIN_PAYLOAD) {
            Block *right=(Block *)((uintptr_t)b+HSIZE+need);
            right->bytes=tail-HSIZE; right->used=0; right->cookie=ARENA_COOKIE;
            link_after(b,right); b->bytes=need; a->used-=tail;
            if(right->next) { if(!block_ok(a,right->next)){fail(a);return 0;} if(!right->next->used)merge_next(right); }
        }
        return ptr;
    }
    if(b->next) {
        Block *n=b->next;
        if(!block_ok(a,n)){fail(a);return 0;}
        if(!n->used && n->bytes<=UINT64_MAX-HSIZE-old) {
            available=old+HSIZE+n->bytes;
            if(available>=need) {
                tail=available-need;
                if(tail>=HSIZE+MIN_PAYLOAD) {
                    Block *right=(Block *)((uintptr_t)b+HSIZE+need);
                    Block *after=n->next; /* right may overlap the old n header */
                    right->bytes=tail-HSIZE; right->used=0; right->cookie=ARENA_COOKIE;
                    right->prev=b; right->next=after; if(after)after->prev=right;
                    b->next=right; b->bytes=need; a->used+=need-old;
                } else {
                    b->bytes=available; b->next=n->next; if(b->next)b->next->prev=b; a->used+=available-old;
                }
                if(a->used>a->peak)a->peak=a->used;
                return ptr;
            }
        }
    }
    q=etaac_arena_alloc(a,16,bytes);
    if(!q)return 0; /* realloc failure preserves the original allocation */
    memcpy(q,ptr,old); etaac_arena_free(a,ptr); return q;
}
int etaac_arena_owns(const ETAACFullArena *a, const void *ptr)
{
    uintptr_t lo,hi,p;
    if(!arena_ok(a)||!ptr)return 0;
    lo=(uintptr_t)a->base; hi=lo+a->bytes; p=(uintptr_t)ptr;
    return p>=lo && p<hi;
}

uint64_t etaac_arena_publish_bytes(ETAACFullArena *a)
{
    uint64_t high=0;
    Block *b;
    if(!arena_ok(a))return 0;
    for(b=a->first;b;b=b->next) {
        uint64_t end;
        if(!block_ok(a,b)){fail(a);return a->bytes;}
        end=(uintptr_t)b-(uintptr_t)a->base+HSIZE+(b->used?b->bytes:0);
        if(end>high)high=end;
    }
    return (high+63)&~UINT64_C(63);
}
