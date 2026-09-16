/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef ETAAC_FULL_RUNTIME_H
#define ETAAC_FULL_RUNTIME_H
#include "protocol.h"
#define ET_CACHE_LINE 64
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ETRuntime ETRuntime;
int ff_et_runtime_open(ETRuntime **out,const char *kernel,char *error,size_t size);
void ff_et_runtime_close(ETRuntime **runtime);
const char *ff_et_runtime_error(const ETRuntime *runtime);
uint64_t ff_et_runtime_shire_mask(const ETRuntime *runtime);
int ff_et_runtime_alloc(ETRuntime *runtime,size_t size,uint64_t *address);
int ff_et_runtime_free(ETRuntime *runtime,uint64_t address);
int ff_et_runtime_write(ETRuntime *runtime,uint64_t dst,const void *src,size_t size);
int ff_et_runtime_read(ETRuntime *runtime,void *dst,uint64_t src,size_t size);
int etaac_full_launch(ETRuntime *runtime,const ETAACFullParams *p,uint64_t mask);
#ifdef __cplusplus
}
#endif
#endif
