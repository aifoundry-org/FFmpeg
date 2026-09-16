/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef ETAAC_FULL_PLATFORM_H
#define ETAAC_FULL_PLATFORM_H
#include "arena.h"
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
void etaac_platform_set_arena(ETAACFullArena *arena);
void etaac_platform_set_firmware_sp(uintptr_t sp);
void etaac_platform_set_escape(jmp_buf *escape);
void etaac_platform_clear_escape(void);
uint64_t etaac_platform_faults(void);
void etaac_platform_reset_faults(void);
uint64_t etaac_platform_ticks(void);
#endif
