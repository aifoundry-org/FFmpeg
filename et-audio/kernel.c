/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "protocol.h"
#include "dsp/synth.h"
#include "../et-kernels/src/cache.h"
#include <string.h>
static int finite_samples(const float *x, unsigned n) {
    for (unsigned i=0;i<n;i++) {
        uint32_t u;
        memcpy(&u, x+i, sizeof(u));
        if ((u & 0x7f800000u)==0x7f800000u) return 0;
    }
    return 1;
}
int etaac_process(const ETAACParams *p, unsigned hart) {
    if (!etaac_params_valid(p) || hart>=64) return ETAAC_BAD_PARAMS;
    unsigned worker=(hart>>1)+32*(hart&1);
    if(worker>=p->active_harts) return 0;
    ETAACInput *inputs=(ETAACInput *)(uintptr_t)p->input_addr;
    ETAACState *states=(ETAACState *)(uintptr_t)p->state_addr;
    ETAACStatus *statuses=(ETAACStatus *)(uintptr_t)p->status_addr;
    float *outputs=(float *)(uintptr_t)p->output_addr;
    float *scratch=(float *)(uintptr_t)p->scratch_addr + worker*ETAAC_SCRATCH_FLOATS;
    float *saved=scratch+4096;
    int result=0;
    for(unsigned i=worker;i<p->count;i+=p->active_harts) {
        const ETAACInput *in=&inputs[i];
        ETAACState *s=&states[i];
        ETAACStatus *st=&statuses[i];
        float *out=outputs+i*ETAAC_SAMPLES;
        int rc=0;
        if(!in->stream_id || in->sequence>3 || in->shape>1 ||
           in->expected_generation==UINT64_MAX ||
           in->expected_generation+1!=p->generation) rc=ETAAC_BAD_INPUT;
        for(unsigned j=0;j<5;j++) if(in->reserved[j]) rc=ETAAC_BAD_INPUT;
        if(s->initialized>1 || s->reserved0 || s->sequence>3 || s->shape>1 ||
           s->generation!=in->expected_generation ||
           (s->initialized && s->stream_id!=in->stream_id)) rc=ETAAC_BAD_STATE;
        for(unsigned j=0;j<4;j++) if(s->reserved[j]) rc=ETAAC_BAD_STATE;
        if(!s->initialized && (s->generation || s->stream_id || s->sequence || s->shape))
            rc=ETAAC_BAD_STATE;
        /* A fresh slot must contain a fully initialized, zero overlap. */
        if(!s->initialized) for(unsigned j=0;j<ETAAC_SAVED;j++)
            if(s->saved[j]!=0.0f) rc=ETAAC_BAD_STATE;
        if(!finite_samples(in->coeff,ETAAC_SAMPLES) ||
           !finite_samples(s->saved,ETAAC_SAVED)) rc=ETAAC_BAD_INPUT;
        if(!rc) {
            memcpy(saved,s->saved,sizeof(s->saved));
            if(p->operation==ETAAC_COPY) memcpy(out,in->coeff,ETAAC_SAMPLES*sizeof(float));
            else if(etaac_synth(out,saved,in->coeff,in->sequence,s->sequence,
                                in->shape,s->shape,scratch)) rc=ETAAC_BAD_INPUT;
            if(!finite_samples(out,ETAAC_SAMPLES) || !finite_samples(saved,ETAAC_SAVED))
                rc=ETAAC_BAD_INPUT;
            if(!rc) {
                memcpy(s->saved,saved,sizeof(s->saved));
                s->stream_id=in->stream_id;
                s->generation=p->generation;
                s->sequence=in->sequence; s->shape=in->shape; s->initialized=1;
                et_evict(out,ETAAC_SAMPLES*sizeof(float));
                et_evict(s,sizeof(*s));
            }
        }
        /* Even errors publish a fresh generation, never partial success. A
         * failed task's old state is preserved; the host must stop the stream. */
        memset(st,0,sizeof(*st));
        st->generation=p->generation; st->stream_id=in->stream_id;
        st->result=rc; st->samples=rc?0:ETAAC_SAMPLES;
        et_evict(st,sizeof(*st));
        if(rc && !result) result=rc;
    }
    return result;
}
#ifdef ET_DEVICE
typedef struct ETKernelEnvironment {
    uint16_t major,minor,patch,reserved;
    uint64_t shire_mask;
    uint32_t frequency,padding;
} ETKernelEnvironment;
int entry_point(const ETAACParams *p,const void *environment) {
    const ETKernelEnvironment *env=environment;
    unsigned long hart;
    __asm__ volatile("csrr %0, hartid" : "=r"(hart));
    if(!env || env->shire_mask!=1 || hart>=64) return ETAAC_BAD_PARAMS;
    return etaac_process(p,(unsigned)hart);
}
#endif
