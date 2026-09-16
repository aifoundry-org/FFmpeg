/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef ETAAC_OFFLINE_PROTOCOL_H
#define ETAAC_OFFLINE_PROTOCOL_H
#include "../protocol.h"
#define ETAAC_OFFLINE_ABI 2u
#define ETAAC_OFFLINE_MAX_FRAMES 512u
/* Frame-major [capacity_frames][count] inputs/outputs. State has count slots.
 * generation is the first requested frame generation, not a launch token.
 * Only one writer owns each stream throughout a launch. */
typedef struct ETAACOfflineParams {
    uint32_t abi_version,count,active_harts,operation;
    uint64_t input_addr,input_bytes,state_addr,state_bytes,output_addr,output_bytes;
    uint64_t scratch_addr,scratch_bytes,status_addr,status_bytes;
    uint64_t generation;
    uint32_t capacity_frames,frame_offset,frames,reserved;
    uint64_t reserved2;
} ETAACOfflineParams;
#ifdef __cplusplus
static_assert(sizeof(ETAACOfflineParams)==128,"offline ABI");
#else
_Static_assert(sizeof(ETAACOfflineParams)==128,"offline ABI");
#endif
static inline int etaac_offline_valid(const ETAACOfflineParams *p) {
    uint64_t a[5],n[5],jobs;
    if(!p || p->abi_version!=ETAAC_OFFLINE_ABI || !p->count || p->count>64 ||
       (p->active_harts!=1 && p->active_harts!=32 && p->active_harts!=64) ||
       p->operation>ETAAC_COPY || !p->generation || !p->frames ||
       !p->capacity_frames || p->capacity_frames>ETAAC_OFFLINE_MAX_FRAMES ||
       p->frames>p->capacity_frames || p->frame_offset>p->capacity_frames-p->frames ||
       p->generation>UINT64_MAX-(p->frames-1) || p->reserved || p->reserved2) return 0;
    jobs=(uint64_t)p->count*p->capacity_frames;
    if(p->input_bytes!=jobs*sizeof(ETAACInput) ||
       p->output_bytes!=jobs*ETAAC_SAMPLES*sizeof(float) ||
       p->state_bytes!=p->count*sizeof(ETAACState) ||
       p->scratch_bytes!=p->active_harts*ETAAC_SCRATCH_FLOATS*sizeof(float) ||
       p->status_bytes!=p->count*sizeof(ETAACStatus))return 0;
    a[0]=p->input_addr;n[0]=p->input_bytes;a[1]=p->state_addr;n[1]=p->state_bytes;
    a[2]=p->output_addr;n[2]=p->output_bytes;a[3]=p->scratch_addr;n[3]=p->scratch_bytes;
    a[4]=p->status_addr;n[4]=p->status_bytes;
    for(unsigned i=0;i<5;i++) {
        if(!a[i] || (a[i]&63) || a[i]>UINT64_MAX-n[i])return 0;
        for(unsigned j=0;j<i;j++)if(a[i]<a[j]+n[j] && a[j]<a[i]+n[i])return 0;
    }
    return 1;
}
int etaac_offline_process(const ETAACOfflineParams *p,unsigned hart);
#endif
