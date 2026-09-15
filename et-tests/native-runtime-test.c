/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Unit test links a counting stub, NOT the decoder. */
#define _POSIX_C_SOURCE 200809L
#include "et_runtime.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned calls, seen[64];
int et_mpeg2_decode(const ETFrameParams *p, unsigned hart)
{
    assert(hart < p->active_harts); ++calls; ++seen[hart]; return 0;
}
int main(void)
{
    ETRuntime *r = NULL; char error[256], src[128], dst[128]; uint64_t a, b, c;
    ETFrameParams p = { 0 };
    unsetenv("FF_ET_TEST_FAIL");
    assert(!ff_et_runtime_open(&r, "NATIVE TEST", error, sizeof(error)));
    assert(ff_et_runtime_shire_mask(r) == 1);
    assert(!ff_et_runtime_alloc(r, sizeof(src), &a) && !(a & 63));
    assert(!ff_et_runtime_alloc(r, sizeof(src), &b) && !(b & 63));
    assert(!ff_et_runtime_alloc(r, sizeof(ETSliceStatus), &c) && !(c & 63));
    memset(src, 0xa5, sizeof(src));
    assert(!ff_et_runtime_write(r, a, src, sizeof(src)));
    assert(!ff_et_runtime_read(r, dst, a, sizeof(dst)));
    assert(!memcmp(src, dst, sizeof(src)));
    assert(ff_et_runtime_write(r, a + 64, src, 128) == -EFAULT);
    assert(ff_et_runtime_write(r, a + 1, src, 64) == -EINVAL);
    assert(ff_et_runtime_read(r, dst, a, 63) == -EINVAL);
    assert(ff_et_runtime_read(r, dst, UINT64_MAX - 63, 64) == -EFAULT);
    p.input_addr = a; p.input_bytes = 128; p.dst_addr = b; p.frame_bytes = 128;
    p.status_addr = c; p.nb_slices = 1; p.active_harts = 64;
    assert(!ff_et_runtime_launch(r, &p, 1)); assert(calls == 64);
    for (unsigned h = 0; h < 64; h++) assert(seen[h] == 1);
    p.active_harts = 1;
    assert(!ff_et_runtime_launch(r, &p, 1)); assert(calls == 65);
    assert(ff_et_runtime_launch(r, &p, 2) == -EINVAL);
    p.active_harts = 65; assert(ff_et_runtime_launch(r, &p, 1) == -EINVAL);
    p.active_harts = 1;
    setenv("FF_ET_TEST_FAIL", "launch", 1);
    assert(ff_et_runtime_launch(r, &p, 1) == -EIO); assert(calls == 65);
    setenv("FF_ET_TEST_FAIL", "read", 1);
    assert(ff_et_runtime_read(r, dst, a, 64) == -EIO);
    setenv("FF_ET_TEST_FAIL", "write", 1);
    assert(ff_et_runtime_write(r, a, src, 64) == -EIO);
    setenv("FF_ET_TEST_FAIL", "alloc", 1);
    { uint64_t x = 1; assert(ff_et_runtime_alloc(r, 64, &x) == -EIO && !x); }
    assert(strstr(ff_et_runtime_error(r), "injected"));
    unsetenv("FF_ET_TEST_FAIL");
    assert(!ff_et_runtime_free(r, a)); assert(ff_et_runtime_free(r, a) == -EINVAL);
    ff_et_runtime_close(&r); assert(!r); ff_et_runtime_close(&r);
    setenv("FF_ET_TEST_FAIL", "open", 1);
    assert(ff_et_runtime_open(&r, NULL, error, sizeof(error)) == -EIO && !r);
    assert(strstr(error, "injected"));
    unsetenv("FF_ET_TEST_FAIL");
    puts("PASS: NATIVE TEST runtime unit test (stub kernel, NO hardware)");
    return 0;
}
