/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef AVCODEC_ET_RUNTIME_H
#define AVCODEC_ET_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include "et_mpeg2_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ETRuntime ETRuntime;

/* Calls return 0 on success and a negative errno on failure. No exceptions
 * cross this boundary. All transfers and launches are synchronous and check
 * both the event and the stream error queue. kernel_path may be NULL for the
 * explicit init-only probe. No implicit CPU or emulator fallback. */
int ff_et_runtime_open(ETRuntime **out, const char *kernel_path,
                       char *error, size_t error_size);
void ff_et_runtime_close(ETRuntime **runtime);
const char *ff_et_runtime_error(const ETRuntime *runtime);
uint64_t ff_et_runtime_shire_mask(const ETRuntime *runtime);
int ff_et_runtime_alloc(ETRuntime *runtime, size_t size, uint64_t *address);
int ff_et_runtime_free(ETRuntime *runtime, uint64_t address);
int ff_et_runtime_write(ETRuntime *runtime, uint64_t dst, const void *src, size_t size);
int ff_et_runtime_read(ETRuntime *runtime, void *dst, uint64_t src, size_t size);
int ff_et_runtime_launch(ETRuntime *runtime, const ETFrameParams *params, uint64_t shire_mask);

#ifdef __cplusplus
}
#endif
#endif
