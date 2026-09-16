/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "protocol.h"
#include "profile_synth.h"
#include "../../et-kernels/src/cache.h"
#include <string.h>
#ifndef ETAAC_FAST_FINITE
#define ETAAC_FAST_FINITE 0
#endif
#if ETAAC_FAST_FINITE
typedef uint32_t alias_u32 __attribute__((may_alias));
#endif
static uint32_t sample_bits(const float *x) {
    uint32_t bits;
#if ETAAC_FAST_FINITE
    bits=*(const alias_u32 *)x;
#else
    memcpy(&bits,x,sizeof(bits));
#endif
    return bits;
}
static int finite_samples(const float *x,unsigned n) {
    for(unsigned i=0;i<n;i++) if((sample_bits(x+i)&UINT32_C(0x7f800000))==UINT32_C(0x7f800000)) return 0;
    return 1;
}
static int input_metadata_valid(const ETAACInput *in,uint64_t expected) {
    if(!in->stream_id||in->sequence>3||in->shape>1||in->expected_generation!=expected)return ETAAC_BAD_INPUT;
    for(unsigned i=0;i<5;i++)if(in->reserved[i])return ETAAC_BAD_INPUT;
    return ETAAC_OK;
}
static int state_valid(const ETAACState *s,const ETAACInput *in,uint64_t expected) {
    int rc=ETAAC_OK;
    if(s->initialized>1||s->reserved0||s->sequence>3||s->shape>1||s->generation!=expected||
       (s->initialized&&s->stream_id!=in->stream_id))rc=ETAAC_BAD_STATE;
    for(unsigned i=0;i<4;i++)if(s->reserved[i])rc=ETAAC_BAD_STATE;
    if(!s->initialized&&(s->generation||s->stream_id||s->sequence||s->shape))rc=ETAAC_BAD_STATE;
    if(!s->initialized)for(unsigned i=0;i<ETAAC_SAVED;i++)if(s->saved[i]!=0.0f)rc=ETAAC_BAD_STATE;
    if(!finite_samples(s->saved,ETAAC_SAVED))rc=ETAAC_BAD_INPUT;
    return rc;
}
static uint64_t phase_begin(unsigned enabled) { return enabled ? et_cycles() : 0; }
static void phase_add(uint64_t *dst,unsigned enabled,uint64_t begin) { if(enabled)*dst+=et_cycles()-begin; }
static void status_write(ETAACProfileStatus *st,uint64_t generation,uint64_t stream,int rc,unsigned operation,unsigned samples,const uint64_t ticks[5]) {
    memset(st,0,sizeof(*st)); st->generation=generation; st->stream_id=stream; st->result=rc;
    st->operation_samples=ETAAC_STATUS_PACK(operation,samples);
    if(ticks)memcpy(st->ticks,ticks,sizeof(st->ticks));
    et_evict(st,sizeof(*st));
}

/* Consumer preconditions deliberately inspect the prior producer status and
 * final state before touching resident PCM.  Input metadata/coefficients are
 * still finite-checked so a consumer request cannot bless a malformed job. */
static int consumer_preconditions(const ETAACProfileParams *p,const ETAACInput *inputs,
                                  const ETAACState *s,const ETAACProfileStatus *prior,
                                  unsigned channel,uint64_t end_generation,uint64_t *stream) {
    const ETAACInput *last=NULL;
    for(unsigned f=0;f<p->frames;f++) {
        const ETAACInput *in=&inputs[(p->frame_offset+f)*p->count+channel];
        int rc=input_metadata_valid(in,p->generation+f-1);
        if(rc||!finite_samples(in->coeff,ETAAC_SAMPLES))return ETAAC_BAD_INPUT;
        if(f==0)*stream=in->stream_id;
        else if(in->stream_id!=*stream)return ETAAC_BAD_STATE;
        last=in;
    }
    if(prior->generation!=end_generation||prior->stream_id!=*stream||prior->result||
       ETAAC_STATUS_OPERATION(prior->operation_samples)!=ETAAC_SYNTH||
       ETAAC_STATUS_SAMPLES(prior->operation_samples)!=p->frames*ETAAC_SAMPLES)return ETAAC_BAD_STATE;
    if(!s->initialized||s->initialized>1||s->reserved0||s->sequence>3||s->shape>1||
       s->stream_id!=*stream||s->generation!=end_generation||s->sequence!=last->sequence||s->shape!=last->shape)return ETAAC_BAD_STATE;
    for(unsigned i=0;i<4;i++)if(s->reserved[i])return ETAAC_BAD_STATE;
    if(!finite_samples(s->saved,ETAAC_SAVED))return ETAAC_BAD_INPUT;
    return ETAAC_OK;
}
static int etaac_profile_consume(const ETAACProfileParams *p,unsigned worker,uint64_t end_generation) {
    ETAACInput *inputs=(ETAACInput *)(uintptr_t)p->input_addr;
    ETAACState *states=(ETAACState *)(uintptr_t)p->state_addr;
    ETAACProfileStatus *statuses=(ETAACProfileStatus *)(uintptr_t)p->status_addr;
    float *outputs=(float *)(uintptr_t)p->output_addr;
    int result=ETAAC_OK;
    for(unsigned i=worker;i<p->count;i+=p->active_harts) {
        ETAACProfileStatus *st=&statuses[i]; uint64_t stream=0,ticks[5]={0,0,0,0,0};
        int rc=consumer_preconditions(p,inputs,&states[i],st,i,end_generation,&stream);
        if(!rc) {
            uint32_t peak=0; uint64_t clipped=0;
            for(unsigned f=0;f<p->frames;f++) {
                const float *pcm=outputs+((size_t)(p->frame_offset+f)*p->count+i)*ETAAC_SAMPLES;
                for(unsigned n=0;n<ETAAC_SAMPLES;n++) {
                    uint32_t magnitude=sample_bits(pcm+n)&UINT32_C(0x7fffffff);
                    if(magnitude>=UINT32_C(0x7f800000)){rc=ETAAC_BAD_INPUT;break;}
                    if(magnitude>peak)peak=magnitude;
                    if(magnitude>UINT32_C(0x3f800000))clipped++;
                }
                if(rc)break;
            }
            ticks[0]=peak; ticks[1]=clipped;
        }
        if(rc)memset(ticks,0,sizeof(ticks));
        status_write(st,end_generation,stream,rc,ETAAC_CONSUME,rc?0:p->frames*ETAAC_SAMPLES,ticks);
        if(rc&&!result)result=rc;
    }
    return result;
}
int etaac_profile_process(const ETAACProfileParams *p,unsigned hart) {
    ETAACInput *inputs; ETAACState *states; ETAACProfileStatus *statuses; float *outputs,*scratch,*saved;
    unsigned worker,i; int result=ETAAC_OK; uint64_t end_generation;
    if(hart>=ETAAC_HARTS || !etaac_profile_valid(p))return ETAAC_BAD_PARAMS;
    end_generation=p->generation+p->frames-1;
    worker=(hart>>1)+32*(hart&1); if(worker>=p->active_harts)return ETAAC_OK;
    if(p->operation==ETAAC_CONSUME)return etaac_profile_consume(p,worker,end_generation);
    inputs=(ETAACInput *)(uintptr_t)p->input_addr; states=(ETAACState *)(uintptr_t)p->state_addr;
    outputs=(float *)(uintptr_t)p->output_addr; scratch=(float *)(uintptr_t)p->scratch_addr+worker*ETAAC_SCRATCH_FLOATS;
    statuses=(ETAACProfileStatus *)(uintptr_t)p->status_addr; saved=scratch+4096;
    for(i=worker;i<p->count;i+=p->active_harts) {
        ETAACState *s=&states[i]; ETAACProfileStatus *st=&statuses[i];
        uint64_t stream_id=s->stream_id,generation=s->generation,status_stream=0;
        unsigned sequence=s->sequence,shape=s->shape,initialized=s->initialized,successful=0,attempted=0,f;
        int rc; ETAACProfileMeasure dsp={0,0,p->profile_enable};
        uint64_t input_ticks=0,output_finite_ticks=0,pcm_publish_ticks=0,phase,ticks[5];
        const unsigned first_index=p->frame_offset*p->count+i; const ETAACInput *first=&inputs[first_index];
        status_stream=first->stream_id; phase=phase_begin(p->profile_enable);
        rc=input_metadata_valid(first,p->generation-1); { int src=state_valid(s,first,p->generation-1); if(src)rc=src; }
        if(!finite_samples(first->coeff,ETAAC_SAMPLES))rc=ETAAC_BAD_INPUT;
        phase_add(&input_ticks,p->profile_enable,phase);
        if(!rc)memcpy(saved,s->saved,sizeof(s->saved));
        for(f=0;!rc&&f<p->frames;f++) {
            const unsigned index=(p->frame_offset+f)*p->count+i; const ETAACInput *in=&inputs[index];
            float *out=outputs+(size_t)index*ETAAC_SAMPLES; const uint64_t expected=p->generation+f-1;
            status_stream=in->stream_id; phase=phase_begin(p->profile_enable);
            rc=input_metadata_valid(in,expected);
            if(generation!=expected||(initialized&&stream_id!=in->stream_id))rc=ETAAC_BAD_STATE;
            if(!finite_samples(in->coeff,ETAAC_SAMPLES))rc=ETAAC_BAD_INPUT;
            phase_add(&input_ticks,p->profile_enable,phase);
            if(rc)break;
            attempted=1;
            if(p->operation==ETAAC_COPY)memcpy(out,in->coeff,ETAAC_SAMPLES*sizeof(*out));
            else if(etaac_profile_synth(out,saved,in->coeff,in->sequence,sequence,in->shape,shape,scratch,&dsp))rc=ETAAC_BAD_INPUT;
            phase=phase_begin(p->profile_enable);
            if(!finite_samples(out,ETAAC_SAMPLES)||!finite_samples(saved,ETAAC_SAVED))rc=ETAAC_BAD_INPUT;
            phase_add(&output_finite_ticks,p->profile_enable,phase);
            if(rc)break;
            stream_id=in->stream_id;generation=p->generation+f;sequence=in->sequence;shape=in->shape;initialized=1;successful++;
            phase=phase_begin(p->profile_enable); et_evict(out,ETAAC_SAMPLES*sizeof(*out)); phase_add(&pcm_publish_ticks,p->profile_enable,phase);
        }
        if(!rc||successful||attempted) {
            memcpy(s->saved,saved,sizeof(s->saved)); s->stream_id=stream_id;s->generation=generation;s->sequence=sequence;s->shape=shape;s->initialized=rc?2:initialized;
            et_evict(s,sizeof(*s));
        }
        ticks[0]=input_ticks; ticks[1]=dsp.imdct_fft_ticks; ticks[2]=dsp.window_overlap_ticks;
        ticks[3]=output_finite_ticks; ticks[4]=pcm_publish_ticks;
        status_write(st,end_generation,status_stream,rc,p->operation,successful*ETAAC_SAMPLES,ticks);
        if(rc&&!result)result=rc;
    } return result;
}
#ifdef ET_DEVICE
typedef struct ETKernelEnvironment { uint16_t major,minor,patch,reserved; uint64_t shire_mask; uint32_t frequency,padding; } ETKernelEnvironment;
int entry_point(const ETAACProfileParams *p,const void *environment) {
    const ETKernelEnvironment *env=environment; unsigned long hart;
    __asm__ volatile("csrr %0, hartid":"=r"(hart));
    if(!env||env->shire_mask!=1||hart>=ETAAC_HARTS)return ETAAC_BAD_PARAMS;
    return etaac_profile_process(p,(unsigned)hart);
}
#endif
