/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "protocol.h"
#include "profile_synth.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void *z(size_t n){void *p=0;assert(!posix_memalign(&p,64,n));memset(p,0,n);return p;}
typedef struct { ETAACInput *in; ETAACState *state; float *out,*scratch; ETAACProfileStatus *status; } B;
static B make(unsigned frames,unsigned count,unsigned harts){B b={z((size_t)frames*count*sizeof(*b.in)),z((size_t)count*sizeof(*b.state)),z((size_t)frames*count*1024*sizeof(*b.out)),z((size_t)harts*ETAAC_SCRATCH_FLOATS*sizeof(*b.scratch)),z((size_t)count*sizeof(*b.status))};return b;}
static void drop(B *b){free(b->in);free(b->state);free(b->out);free(b->scratch);free(b->status);}
static void fill(ETAACInput *p,unsigned ch,unsigned f){memset(p,0,sizeof(*p));p->stream_id=ch+1;p->expected_generation=f;p->sequence=f&3;p->shape=(ch+f)&1;for(unsigned j=0;j<1024;j++)p->coeff[j]=(float)((int)((j*719u+ch*23u+f*373u)&65535u)-32768);}
static ETAACProfileParams params(const B *b,unsigned frames,unsigned count,unsigned harts,unsigned enabled){ETAACProfileParams p={0};p.abi_version=ETAAC_PROFILE_ABI;p.count=count;p.active_harts=harts;p.generation=1;p.capacity_frames=frames;p.frames=frames;p.profile_enable=enabled;p.input_addr=(uintptr_t)b->in;p.input_bytes=(uint64_t)frames*count*sizeof(*b->in);p.state_addr=(uintptr_t)b->state;p.state_bytes=(uint64_t)count*sizeof(*b->state);p.output_addr=(uintptr_t)b->out;p.output_bytes=(uint64_t)frames*count*1024*sizeof(float);p.scratch_addr=(uintptr_t)b->scratch;p.scratch_bytes=(uint64_t)harts*ETAAC_SCRATCH_FLOATS*sizeof(float);p.status_addr=(uintptr_t)b->status;p.status_bytes=(uint64_t)count*sizeof(*b->status);return p;}
static int harts(const ETAACProfileParams *p){int rc=0;for(unsigned h=0;h<64;h++){int n=etaac_profile_process(p,h);if(n&&!rc)rc=n;}return rc;}
static void exact_and_control(void){ enum{C=64,F=4,H=64};B off=make(F,C,H),on=make(F,C,H);B ref=make(F,C,H);for(unsigned f=0;f<F;f++)for(unsigned i=0;i<C;i++){fill(&off.in[f*C+i],i,f);memcpy(&on.in[f*C+i],&off.in[f*C+i],sizeof(*off.in));}
 ETAACProfileParams p=params(&off,F,C,H,0),q=params(&on,F,C,H,1);assert(etaac_profile_valid(&p)&&etaac_profile_valid(&q));assert(!harts(&p));assert(!harts(&q));
 for(unsigned f=0;f<F;f++)for(unsigned i=0;i<C;i++){ETAACInput *in=&off.in[f*C+i];assert(!etaac_synth(ref.out+((size_t)f*C+i)*1024,ref.state[i].saved,in->coeff,in->sequence,ref.state[i].sequence,in->shape,ref.state[i].shape,ref.scratch));ref.state[i].stream_id=i+1;ref.state[i].generation=f+1;ref.state[i].sequence=in->sequence;ref.state[i].shape=in->shape;ref.state[i].initialized=1;}
 assert(!memcmp(off.out,on.out,(size_t)F*C*1024*sizeof(float)));assert(!memcmp(off.out,ref.out,(size_t)F*C*1024*sizeof(float)));assert(!memcmp(off.state,on.state,(size_t)C*sizeof(*off.state)));assert(!memcmp(off.state,ref.state,(size_t)C*sizeof(*off.state)));
 for(unsigned i=0;i<C;i++){assert(off.status[i].result==0&&ETAAC_STATUS_OPERATION(off.status[i].operation_samples)==ETAAC_SYNTH&&ETAAC_STATUS_SAMPLES(off.status[i].operation_samples)==F*1024);for(unsigned k=0;k<5;k++)assert(!off.status[i].ticks[k]&&!on.status[i].ticks[k]);}
 p.operation=ETAAC_CONSUME;assert(!harts(&p));for(unsigned i=0;i<C;i++){uint64_t peak=0,clips=0;for(unsigned f=0;f<F;f++)for(unsigned n=0;n<1024;n++){uint32_t bits;memcpy(&bits,off.out+((size_t)f*C+i)*1024+n,4);bits&=UINT32_C(0x7fffffff);if(bits>peak)peak=bits;if(bits>UINT32_C(0x3f800000))clips++;}assert(!off.status[i].result&&ETAAC_STATUS_OPERATION(off.status[i].operation_samples)==ETAAC_CONSUME&&off.status[i].ticks[0]==peak&&off.status[i].ticks[1]==clips);}
 drop(&off);drop(&on);drop(&ref);}
static void consumer_rejections(void){B b=make(1,1,1);fill(b.in,0,0);ETAACProfileParams p=params(&b,1,1,1,0);assert(!harts(&p));ETAACProfileStatus good=*b.status;ETAACState state=*b.state;p.operation=ETAAC_CONSUME;
#define CONSUMER_FAIL(code) do{assert(harts(&p)==(code));assert(ETAAC_STATUS_OPERATION(b.status->operation_samples)==ETAAC_CONSUME&&ETAAC_STATUS_SAMPLES(b.status->operation_samples)==0);}while(0)
 *b.status=good;b.status->generation--;CONSUMER_FAIL(ETAAC_BAD_STATE);
 *b.status=good;b.status->stream_id++;CONSUMER_FAIL(ETAAC_BAD_STATE);
 *b.status=good;b.status->operation_samples=ETAAC_STATUS_PACK(ETAAC_COPY,1024);CONSUMER_FAIL(ETAAC_BAD_STATE);
 *b.status=good;b.status->operation_samples=ETAAC_STATUS_PACK(ETAAC_SYNTH,0);CONSUMER_FAIL(ETAAC_BAD_STATE);
 *b.status=good;*b.state=state;b.state->initialized=2;CONSUMER_FAIL(ETAAC_BAD_STATE);
 *b.status=good;*b.state=state;uint32_t nan=UINT32_C(0x7fc00000);memcpy(b.out,&nan,4);CONSUMER_FAIL(ETAAC_BAD_INPUT);
 *b.status=good;*b.state=state;memset(b.out,0,1024*sizeof(*b.out));uint32_t v[]={UINT32_C(0x80000000),0,UINT32_C(0x3f800000),UINT32_C(0xbf800000),UINT32_C(0x3f800001),UINT32_C(0xbf800001)};memcpy(b.out,v,sizeof(v));assert(!harts(&p));assert(b.status->ticks[0]==UINT32_C(0x3f800001)&&b.status->ticks[1]==2&&ETAAC_STATUS_SAMPLES(b.status->operation_samples)==1024);
#undef CONSUMER_FAIL
 drop(&b);}
static void reject(void){B b=make(1,1,1);fill(b.in,0,0);ETAACProfileParams p=params(&b,1,1,1,0),x=p;assert(etaac_profile_valid(&p));x.abi_version=2;assert(!etaac_profile_valid(&x)&&etaac_profile_process(&x,0)==ETAAC_BAD_PARAMS);x=p;x.profile_enable=2;assert(!etaac_profile_valid(&x));p.operation=ETAAC_CONSUME;assert(etaac_profile_process(&p,0)==ETAAC_BAD_STATE&&ETAAC_STATUS_OPERATION(b.status->operation_samples)==ETAAC_CONSUME);p.operation=ETAAC_SYNTH;uint32_t nan=0x7fc00000;memcpy(b.in->coeff,&nan,4);assert(etaac_profile_process(&p,0)==ETAAC_BAD_INPUT&&b.status->result==ETAAC_BAD_INPUT);drop(&b);}
int main(void){exact_and_control();consumer_rejections();reject();puts("PASS profile native: ABI3 exact DSP/control, consumer guards/meter, finite and bounds");return 0;}
