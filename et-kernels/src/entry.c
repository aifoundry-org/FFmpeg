/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "decoder_internal.h"
/* GP-SDK kernel environment ABI (four uint16 version fields, shire mask).
 * Launch on precisely ONE compute shire. Relative IDs remain 0..63 even when
 * the runtime selects a nonzero physical shire. No runtime/uber dependency. */
typedef struct ETKernelEnvironment {
    uint16_t major, minor, patch, reserved;
    uint64_t shire_mask;
    uint32_t frequency, padding2;
} ETKernelEnvironment;
int entry_point(const ETFrameParams *params, const void *environment)
{
    const ETKernelEnvironment *env = environment;
    unsigned long hart;
    __asm__ volatile("csrr %0, hartid" : "=r"(hart));
    if (!params) return ET_DECODE_BAD_PARAMS;
    if (!env || !env->shire_mask)
        return et_mpeg2_report_failure(params, hart & 63, ET_DECODE_BAD_PARAMS);
    unsigned shire = 0;
    while (!((env->shire_mask >> shire) & 1)) shire++;
    if (env->shire_mask & (env->shire_mask-1))
        return et_mpeg2_report_failure(params, hart == shire*64 ? 0 : 1, ET_DECODE_BAD_PARAMS);
    if (hart < shire*64 || hart >= (shire+1)*64)
        return et_mpeg2_report_failure(params, hart & 63, ET_DECODE_BAD_PARAMS);
    return et_mpeg2_decode(params, hart-shire*64);
}
