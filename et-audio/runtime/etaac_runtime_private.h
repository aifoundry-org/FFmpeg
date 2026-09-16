/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef ETAAC_RUNTIME_PRIVATE_H
#define ETAAC_RUNTIME_PRIVATE_H

#include <stddef.h>
#include <stdint.h>
#include "../protocol.h"

/* The copied shim uses this existing internal spelling.  It is private to the
 * independently linked AAC runtime and is not a video protocol dependency. */
#define ET_CACHE_LINE 64

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ETRuntime ETRuntime;
int ff_et_runtime_open(ETRuntime **out, const char *kernel_path,
                       char *error, size_t error_size);
void ff_et_runtime_close(ETRuntime **runtime);
const char *ff_et_runtime_error(const ETRuntime *runtime);
uint64_t ff_et_runtime_shire_mask(const ETRuntime *runtime);
int ff_et_runtime_alloc(ETRuntime *runtime, size_t size, uint64_t *address);
int ff_et_runtime_free(ETRuntime *runtime, uint64_t address);
int ff_et_runtime_write(ETRuntime *runtime, uint64_t dst, const void *src, size_t size);
int ff_et_runtime_read(ETRuntime *runtime, void *dst, uint64_t src, size_t size);
int etaac_rt_launch(ETRuntime *runtime, const ETAACParams *params,
                    uint64_t shire_mask);
#ifdef __cplusplus
}
#endif
#endif
