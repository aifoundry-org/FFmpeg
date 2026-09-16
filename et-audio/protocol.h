/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef ET_AUDIO_PROTOCOL_H
#define ET_AUDIO_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>
#define ETAAC_ABI 1u
#define ETAAC_MAX_TASKS 64u
#define ETAAC_HARTS 64u
#define ETAAC_SAMPLES 1024u
#define ETAAC_SAVED 512u
/* 4096 DSP scratch floats plus a transactional 512-float saved-state copy. */
#define ETAAC_SCRATCH_FLOATS 4608u
#define ETAAC_SYNTH 0u
#define ETAAC_COPY 1u
#define ETAAC_OK 0
#define ETAAC_BAD_PARAMS (-1)
#define ETAAC_BAD_STATE (-2)
#define ETAAC_BAD_INPUT (-3)
/* Fixed slots are independent CHANNEL states, not arbitrary AAC packets.
 * A launch contains one next synthesis frame for each slot [0,count).
 * All buffers/ranges must be disjoint and 64-byte aligned. */
typedef struct ETAACParams {
    uint32_t abi_version, count, active_harts, operation;
    uint64_t input_addr, input_bytes;
    uint64_t state_addr, state_bytes;
    uint64_t output_addr, output_bytes;
    uint64_t scratch_addr, scratch_bytes;
    uint64_t status_addr, status_bytes;
    uint64_t generation, reserved[3];
} ETAACParams;
typedef struct ETAACInput {
    uint64_t stream_id;
    uint32_t sequence, shape;
    uint64_t expected_generation, reserved[5];
    float coeff[ETAAC_SAMPLES];
} ETAACInput;
typedef struct ETAACState {
    uint64_t stream_id, generation;
    uint32_t sequence, shape, initialized, reserved0;
    uint64_t reserved[4];
    float saved[ETAAC_SAVED];
} ETAACState;
typedef struct ETAACStatus {
    uint64_t generation, stream_id;
    int32_t result;
    uint32_t samples;
    uint64_t reserved[5];
} ETAACStatus;
#ifdef __cplusplus
static_assert(sizeof(ETAACParams)==128 && sizeof(ETAACInput)==4160 &&
              sizeof(ETAACState)==2112 && sizeof(ETAACStatus)==64, "audio ABI");
#else
_Static_assert(sizeof(ETAACParams)==128 && sizeof(ETAACInput)==4160 &&
               sizeof(ETAACState)==2112 && sizeof(ETAACStatus)==64, "audio ABI");
#endif
/* Pure structural validation shared by host and device. Owned allocation checks
 * additionally belong to the runtime; a valid shape is not an address capability. */
static inline int etaac_params_valid(const ETAACParams *p) {
    uint64_t a[5], n[5];
    unsigned i,j;
    if (!p || p->abi_version != ETAAC_ABI || !p->count || p->count > ETAAC_MAX_TASKS ||
        (p->active_harts != 1 && p->active_harts != 32 && p->active_harts != 64) ||
        p->operation > ETAAC_COPY || !p->generation ||
        p->reserved[0] || p->reserved[1] || p->reserved[2]) return 0;
    a[0]=p->input_addr; n[0]=p->input_bytes;
    a[1]=p->state_addr; n[1]=p->state_bytes;
    a[2]=p->output_addr; n[2]=p->output_bytes;
    a[3]=p->scratch_addr; n[3]=p->scratch_bytes;
    a[4]=p->status_addr; n[4]=p->status_bytes;
    if (n[0] != p->count * sizeof(ETAACInput) ||
        n[1] != p->count * sizeof(ETAACState) ||
        n[2] != p->count * ETAAC_SAMPLES * sizeof(float) ||
        n[3] != p->active_harts * ETAAC_SCRATCH_FLOATS * sizeof(float) ||
        n[4] != p->count * sizeof(ETAACStatus)) return 0;
    for(i=0;i<5;i++) {
        if(!a[i] || (a[i]&63) || (n[i]&63) || a[i]>UINT64_MAX-n[i]) return 0;
        for(j=0;j<i;j++) if(a[i]<a[j]+n[j] && a[j]<a[i]+n[i]) return 0;
    }
    return 1;
}
int etaac_process(const ETAACParams *params, unsigned hart);
#endif
