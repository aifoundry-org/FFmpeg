/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef ETAAC_FULL_ARENA_H
#define ETAAC_FULL_ARENA_H
#include <stddef.h>
#include <stdint.h>

typedef struct ETAACFullArena ETAACFullArena;
struct ETAACFullArena {
    uint8_t *base;
    uint64_t bytes, peak, used, failures;
    void *first;
    uint32_t cookie;
};
void etaac_arena_init(ETAACFullArena *a, void *base, uint64_t bytes);
void *etaac_arena_alloc(ETAACFullArena *a, size_t alignment, size_t bytes);
void *etaac_arena_realloc(ETAACFullArena *a, void *ptr, size_t bytes);
void etaac_arena_free(ETAACFullArena *a, void *ptr);
int etaac_arena_owns(const ETAACFullArena *a, const void *ptr);
/* Prefix containing all live payload and allocator headers, rounded to lines. */
uint64_t etaac_arena_publish_bytes(ETAACFullArena *a);
#endif
