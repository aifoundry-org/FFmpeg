/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "platform.h"
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#ifndef ET_DEVICE
#include <stdlib.h>
#endif
static ETAACFullArena *g_arena = 0;
static jmp_buf *g_escape = 0;
static uint64_t g_faults = 0;
static int g_errno = 0;
static uintptr_t g_firmware_sp = 0;
static int valid_align(size_t a) { return a && !(a&(a-1)) && a<=4096; }
static void fault(void) { g_faults++; g_errno=ENOSYS; }
void etaac_platform_set_arena(ETAACFullArena *a) { g_arena=a; }
void etaac_platform_set_firmware_sp(uintptr_t sp) { g_firmware_sp=sp; }
void etaac_platform_set_escape(jmp_buf *j) { g_escape=j; }
void etaac_platform_clear_escape(void) { g_escape=0; }
uint64_t etaac_platform_faults(void) { return g_faults; }
void etaac_platform_reset_faults(void) { g_faults=0; }
uint64_t etaac_platform_ticks(void) { return 0; } /* no user-readable timer CSR */
int *__errno(void) { return &g_errno; }
void *malloc(size_t n) { return g_arena ? etaac_arena_alloc(g_arena,16,n) : 0; }
void *calloc(size_t n, size_t s) { if(s && n>SIZE_MAX/s)return 0; void *p=malloc(n*s); if(p)memset(p,0,n*s); return p; }
void free(void *p) { if(g_arena)etaac_arena_free(g_arena,p); }
void *realloc(void *p,size_t n) { return g_arena ? etaac_arena_realloc(g_arena,p,n) : 0; }
void *memalign(size_t a,size_t n) { return g_arena ? etaac_arena_alloc(g_arena,a,n) : 0; }
int posix_memalign(void **p,size_t a,size_t n) { void *q; if(!p || a<sizeof(void *) || !valid_align(a))return EINVAL; q=memalign(a,n); if(!q)return ENOMEM;*p=q;return 0; }
void *aligned_alloc(size_t a,size_t n) { if(!valid_align(a) || !n || n%a){ g_errno=EINVAL; return 0; } return memalign(a,n); }
/* etaac_full_process arms an owned setjmp escape before every reachable
 * FFmpeg/newlib call. Returning from abort is undefined, so it is deliberately
 * noreturn and cannot silently continue after allocator/library corruption. */
__attribute__((noreturn)) void abort(void) {
    fault();
    if(g_escape) longjmp(*g_escape,1);
#ifdef ET_DEVICE
    /* Emergency path: return normally to firmware, on its original stack. */
    if(g_firmware_sp) {
        __asm__ volatile("mv sp, %0\n\tli a1, -7\n\tli a2, 0\n\tli a0, 8\n\tecall" :: "r"(g_firmware_sp) : "a0","a1","a2","memory");
    }
    /* Firmware return is specified non-returning. A missing SP is unreachable
     * after entry validation and never permits abort to return into FFmpeg. */
    __builtin_unreachable();
#else
    _Exit(EXIT_FAILURE);
#endif
}
int _write(int fd, const void *buf, size_t n) { (void)fd;(void)buf;(void)n;fault();return -1; }
int _read(int fd, void *buf, size_t n) { (void)fd;(void)buf;(void)n;fault();return -1; }
int _close(int fd) { (void)fd;fault();return -1; }
int _open(const char *p,int f,int m) { (void)p;(void)f;(void)m;fault();return -1; }
int _fcntl(int fd,int cmd,...) { (void)fd;(void)cmd;fault();return -1; }
int _fstat(int fd, struct stat *st) { (void)fd;(void)st;fault();return -1; }
int _stat(const char *p, struct stat *st) { (void)p;(void)st;fault();return -1; }
clock_t _times(struct tms *t) { (void)t;fault();return (clock_t)-1; }
int _gettimeofday(struct timeval *tv, void *tz) { (void)tv;(void)tz;fault();return -1; }
int _isatty(int fd) { (void)fd;fault();return 0; }
long _lseek(int fd,long o,int w) { (void)fd;(void)o;(void)w;fault();return -1; }
int _getpid(void) { fault();return -1; }
int _kill(int p,int s) { (void)p;(void)s;fault();return -1; }
void *_sbrk(ptrdiff_t n) { (void)n;fault();return (void *)-1; }
