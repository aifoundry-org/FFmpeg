/* SPDX-License-Identifier: LGPL-2.1-or-later
 * HOST-NATIVE TEST ONLY. Never compile into the production ET runtime.
 */
#define _POSIX_C_SOURCE 200112L
#include "et_runtime.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int et_mpeg2_decode(const ETFrameParams *, unsigned hart);
struct Allocation { void *ptr; size_t size; struct Allocation *next; };
struct ETRuntime { struct Allocation *allocs; char error[256]; unsigned calls[6]; };
static int fail(ETRuntime *r, int code, const char *msg)
{
    if (r) snprintf(r->error, sizeof(r->error), "NATIVE TEST: %s", msg);
    return -code;
}
/* FF_ET_TEST_FAIL=alloc|write|read|launch|free[:N] (1-based invocation). */
static int injected(ETRuntime *r, unsigned op)
{
    static const char *names[] = {"alloc", "write", "read", "launch", "free"};
    const char *s = getenv("FF_ET_TEST_FAIL");
    unsigned call = ++r->calls[op];
    size_t n = strlen(names[op]);
    if (s && !strncmp(s, names[op], n) &&
        (!s[n] || (s[n] == ':' && call == strtoul(s+n+1, NULL, 10)))) {
        fprintf(stderr, "NATIVE TEST: injected %s failure (call %u)\n", names[op], call);
        return fail(r, EIO, "injected runtime error");
    }
    return 0;
}
static int contains(ETRuntime *r, uint64_t addr, size_t bytes)
{
    struct Allocation *a;
    for (a = r->allocs; a; a = a->next) {
        uint64_t base = (uintptr_t)a->ptr;
        if (addr >= base && addr-base <= a->size && bytes <= a->size-(addr-base)) return 1;
    }
    return 0;
}
int ff_et_runtime_open(ETRuntime **out, const char *kernel_path, char *error, size_t size)
{
    const char *fault = getenv("FF_ET_TEST_FAIL");
    (void)kernel_path;
    fprintf(stderr, "*** NATIVE TEST RUNTIME: CPU execution only; NO ET hardware or simulator ***\n");
    if (!out) return -EINVAL;
    *out = NULL;
    if (fault && !strcmp(fault, "open")) {
        if (error && size) snprintf(error, size, "NATIVE TEST: injected open failure");
        return -EIO;
    }
    *out = calloc(1, sizeof(**out));
    if (!*out) {
        if (error && size) snprintf(error, size, "NATIVE TEST: out of memory");
        return -ENOMEM;
    }
    if (error && size) error[0] = 0;
    return 0;
}
void ff_et_runtime_close(ETRuntime **pr)
{
    struct Allocation *a;
    if (!pr || !*pr) return;
    while ((a = (*pr)->allocs)) { (*pr)->allocs = a->next; free(a->ptr); free(a); }
    free(*pr); *pr = NULL;
}
const char *ff_et_runtime_error(const ETRuntime *r) { return r ? r->error : "NATIVE TEST: null runtime"; }
uint64_t ff_et_runtime_shire_mask(const ETRuntime *r) { return r ? UINT64_C(1) : 0; }
int ff_et_runtime_alloc(ETRuntime *r, size_t size, uint64_t *address)
{
    struct Allocation *a; int rc;
    if (address) *address = 0;
    if (!r || !address || !size || size > SIZE_MAX - 63)
        return fail(r, EINVAL, "invalid allocation");
    size = (size + 63) & ~(size_t)63;
    if ((rc = injected(r, 0))) return rc;
    a = calloc(1, sizeof(*a));
    if (!a) return fail(r, ENOMEM, "allocation metadata");
    rc = posix_memalign(&a->ptr, 64, size);
    if (rc) { free(a); return fail(r, rc, "aligned allocation"); }
    memset(a->ptr, 0, size); a->size = size; a->next = r->allocs; r->allocs = a;
    *address = (uintptr_t)a->ptr; return 0;
}
int ff_et_runtime_free(ETRuntime *r, uint64_t address)
{
    struct Allocation **p; int rc;
    if (!r) return -EINVAL;
    if ((rc = injected(r, 4))) return rc;
    if (!address) return 0;
    for (p = &r->allocs; *p; p = &(*p)->next) if ((uintptr_t)(*p)->ptr == address) {
        struct Allocation *a = *p; *p = a->next; free(a->ptr); free(a); return 0;
    }
    return fail(r, EINVAL, "free of unknown address");
}
int ff_et_runtime_write(ETRuntime *r, uint64_t dst, const void *src, size_t size)
{
    int rc;
    if (!r) return -EINVAL;
    if ((rc = injected(r, 1))) return rc;
    if (!size) return 0;
    if (!src || (dst & 63) || (size & 63)) return fail(r, EINVAL, "unaligned DMA write");
    if (!contains(r, dst, size)) return fail(r, EFAULT, "write outside allocation");
    if (size) memcpy((void *)(uintptr_t)dst, src, size);
    return 0;
}
int ff_et_runtime_read(ETRuntime *r, void *dst, uint64_t src, size_t size)
{
    int rc;
    if (!r) return -EINVAL;
    if ((rc = injected(r, 2))) return rc;
    if (!size) return 0;
    if (!dst || (src & 63) || (size & 63)) return fail(r, EINVAL, "unaligned DMA read");
    if (!contains(r, src, size)) return fail(r, EFAULT, "read outside allocation");
    if (size) memcpy(dst, (void *)(uintptr_t)src, size);
    return 0;
}
int ff_et_runtime_launch(ETRuntime *r, const ETFrameParams *p, uint64_t mask)
{
    int rc; unsigned h;
    if (!r || !p) return -EINVAL;
    if ((rc = injected(r, 3))) return rc;
    if (mask != 1 || !p->active_harts || p->active_harts > ET_MPEG2_HARTS ||
        !p->nb_slices || p->nb_slices > ET_MPEG2_MAX_SLICES ||
        !contains(r, p->input_addr, p->input_bytes) ||
        !contains(r, p->dst_addr, p->frame_bytes) ||
        !contains(r, p->status_addr, p->nb_slices * sizeof(ETSliceStatus)) ||
        (p->ref_fwd_addr && !contains(r, p->ref_fwd_addr, p->frame_bytes)) ||
        (p->ref_bwd_addr && !contains(r, p->ref_bwd_addr, p->frame_bytes)))
        return fail(r, EINVAL, "invalid launch addresses, harts, or shire mask");
    for (h = 0; h < p->active_harts; h++) et_mpeg2_decode(p, h);
    return 0;
}
