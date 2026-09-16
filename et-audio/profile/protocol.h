/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef ETAAC_PROFILE_PROTOCOL_H
#define ETAAC_PROFILE_PROTOCOL_H
#include "../protocol.h"
#define ETAAC_PROFILE_ABI 3u
#define ETAAC_PROFILE_MAX_FRAMES 512u
#define ETAAC_CONSUME 2u
#define ETAAC_STATUS_OP_SHIFT 30u
#define ETAAC_STATUS_SAMPLES_MASK ((UINT32_C(1)<<ETAAC_STATUS_OP_SHIFT)-1)
#define ETAAC_STATUS_PACK(operation,samples) (((uint32_t)(operation)<<ETAAC_STATUS_OP_SHIFT)|(uint32_t)(samples))
#define ETAAC_STATUS_OPERATION(tag) ((unsigned)((tag)>>ETAAC_STATUS_OP_SHIFT))
#define ETAAC_STATUS_SAMPLES(tag) ((unsigned)((tag)&ETAAC_STATUS_SAMPLES_MASK))
typedef struct ETAACProfileParams {
    uint32_t abi_version,count,active_harts,operation;
    uint64_t input_addr,input_bytes,state_addr,state_bytes,output_addr,output_bytes;
    uint64_t scratch_addr,scratch_bytes,status_addr,status_bytes;
    uint64_t generation;
    uint32_t capacity_frames,frame_offset,frames,profile_enable;
    uint64_t reserved2;
} ETAACProfileParams;
typedef struct ETAACProfileStatus {
    uint64_t generation,stream_id;
    int32_t result;
    /* operation is explicitly tagged with the completed sample count.  For
     * ETAAC_CONSUME, ticks[0]=peak positive IEEE magnitude bits and
     * ticks[1]=count(|PCM| > 1.0f); the remaining slots are zero. */
    uint32_t operation_samples;
    uint64_t ticks[5];
} ETAACProfileStatus;
#ifdef __cplusplus
static_assert(sizeof(ETAACProfileParams)==128 && sizeof(ETAACProfileStatus)==64,"profile ABI 3");
#else
_Static_assert(sizeof(ETAACProfileParams)==128 && sizeof(ETAACProfileStatus)==64,"profile ABI 3");
#endif
static inline int etaac_profile_valid(const ETAACProfileParams *p) {
    uint64_t a[5],n[5],jobs;
    if(!p || p->abi_version!=ETAAC_PROFILE_ABI || !p->count || p->count>64 ||
       (p->active_harts!=1 && p->active_harts!=32 && p->active_harts!=64) ||
       p->operation>ETAAC_CONSUME || !p->generation || !p->frames ||
       !p->capacity_frames || p->capacity_frames>ETAAC_PROFILE_MAX_FRAMES ||
       p->frames>p->capacity_frames || p->frame_offset>p->capacity_frames-p->frames ||
       p->generation>UINT64_MAX-(p->frames-1) || p->profile_enable>1 || p->reserved2) return 0;
    jobs=(uint64_t)p->count*p->capacity_frames;
    if(p->input_bytes!=jobs*sizeof(ETAACInput) || p->output_bytes!=jobs*ETAAC_SAMPLES*sizeof(float) ||
       p->state_bytes!=p->count*sizeof(ETAACState) || p->scratch_bytes!=p->active_harts*ETAAC_SCRATCH_FLOATS*sizeof(float) ||
       p->status_bytes!=p->count*sizeof(ETAACProfileStatus))return 0;
    a[0]=p->input_addr;n[0]=p->input_bytes;a[1]=p->state_addr;n[1]=p->state_bytes;
    a[2]=p->output_addr;n[2]=p->output_bytes;a[3]=p->scratch_addr;n[3]=p->scratch_bytes;a[4]=p->status_addr;n[4]=p->status_bytes;
    for(unsigned i=0;i<5;i++){if(!a[i]||(a[i]&63)||a[i]>UINT64_MAX-n[i])return 0;for(unsigned j=0;j<i;j++)if(a[i]<a[j]+n[j]&&a[j]<a[i]+n[i])return 0;}
    return 1;
}
int etaac_profile_process(const ETAACProfileParams *p,unsigned hart);
#endif
