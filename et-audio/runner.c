/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "protocol.h"
#include "dsp/synth.h"
#include "runtime/etaac_runtime.h"
#include "capture_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <errno.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
static void *zeros(size_t n) { void *p=NULL; if(posix_memalign(&p,64,n)) return NULL; memset(p,0,n); return p; }
static int number(const char *s,unsigned *v) {
    char *e; errno=0; unsigned long n=strtoul(s,&e,10);
    if(errno || !*s || *e || n>1000000) return -1;
    *v=(unsigned)n; return 0;
}
static uint32_t rng(uint32_t *s) { *s^=*s<<13; *s^=*s>>17; *s^=*s<<5; return *s; }
#define RT(call) do { if((rc=(call))<0) { fprintf(stderr,"%s: %s (%d)\n",#call,ff_et_runtime_error(rt),rc); goto done; } } while(0)
int main(int argc,char **argv) {
    unsigned count,harts,frames,operation;
    if((argc!=6 && argc!=7) || number(argv[2],&count) || number(argv[3],&harts) ||
       number(argv[4],&frames) || number(argv[5],&operation) || !frames || frames>1000 ||
       !count || count>64 || (harts!=1 && harts!=32 && harts!=64) || operation>1) {
        fprintf(stderr,"usage: etaac-run KERNEL TASKS HARTS FRAMES OP(0=synthesis,1=copy) [CAPTURE]\n"); return 2;
    }
    if(!getenv("ETAAC_HW_GUARD") || strcmp(getenv("ETAAC_HW_GUARD"),"1") ||
       !getenv("FF_ET_SYSEMU") || strcmp(getenv("FF_ET_SYSEMU"),"0")) {
        fprintf(stderr,"Use et-audio/run-silicon.sh: no unguarded device initialization\n"); return 2;
    }
    ETRuntime *rt=NULL; int rc=0,ok=0; char error[512];
    ETAACCapture capture={0};
    ETAACParams p={0}; p.abi_version=ETAAC_ABI;p.count=count;p.active_harts=harts;p.operation=operation;
    p.input_bytes=count*sizeof(ETAACInput);p.state_bytes=count*sizeof(ETAACState);
    p.output_bytes=count*ETAAC_SAMPLES*sizeof(float);p.status_bytes=count*sizeof(ETAACStatus);
    p.scratch_bytes=harts*ETAAC_SCRATCH_FLOATS*sizeof(float);
    ETAACInput *input=zeros(p.input_bytes);
    ETAACState *states=zeros(p.state_bytes), *expected=zeros(p.state_bytes);
    ETAACStatus *status=zeros(p.status_bytes);
    float *output=zeros(p.output_bytes),*gold=zeros(p.output_bytes),*scratch=zeros(ETAAC_SCRATCH_FLOATS*sizeof(float));
    if(!input||!states||!expected||!status||!output||!gold||!scratch) { rc=-1; goto done; }
    if(argc==7) {
        if(capture_load(&capture,argv[6])) {fprintf(stderr,"invalid capture file\n");rc=-1;goto done;}
        if(capture_select(&capture,frames)) {fprintf(stderr,"capture too short\n");rc=-1;goto done;}
        for(unsigned i=0;i<count;i++) {
            const ETAACRecord *r=capture_record(&capture,i,0);
            states[i].initialized=1;states[i].stream_id=i+1;
            states[i].sequence=r->previous_sequence;states[i].shape=r->previous_shape;
            memcpy(states[i].saved,r->before,sizeof(states[i].saved));
        }
        memcpy(expected,states,p.state_bytes);
    }
    double start=now();
    rc=ff_et_runtime_open(&rt,argv[1],error,sizeof(error));
    if(rc<0) { fprintf(stderr,"open: %s\n",error);goto done; }
    if(!(ff_et_runtime_shire_mask(rt)&1)) { rc=-1;goto done; }
    RT(ff_et_runtime_alloc(rt,p.input_bytes,&p.input_addr));
    RT(ff_et_runtime_alloc(rt,p.state_bytes,&p.state_addr));
    RT(ff_et_runtime_alloc(rt,p.output_bytes,&p.output_addr));
    RT(ff_et_runtime_alloc(rt,p.status_bytes,&p.status_addr));
    RT(ff_et_runtime_alloc(rt,p.scratch_bytes,&p.scratch_addr));
    RT(ff_et_runtime_write(rt,p.state_addr,states,p.state_bytes));
    double setup=now()-start;
    printf("{\"type\":\"config\",\"backend\":\"silicon-shire0\",\"tasks\":%u,\"harts\":%u,\"frames\":%u,\"operation\":%u,\"capture_contexts\":%u,\"setup_s\":%.9f}\n",count,harts,frames,operation,capture.selected_count,setup);
    uint32_t seed=0x7654321u;
    unsigned sequences[]={0,1,2,2,3,0};
    for(unsigned f=0;f<frames;f++) {
        p.generation=f+1;
        double c0=now();
        for(unsigned i=0;i<count;i++) {
            input[i].stream_id=i+1;input[i].expected_generation=f;
            if(capture.contexts) {
                const ETAACRecord *r=capture_record(&capture,i,f);
                input[i].sequence=r->sequence;input[i].shape=r->shape;
                memcpy(input[i].coeff,r->coeff,sizeof(input[i].coeff));
            } else {
                input[i].sequence=sequences[f%6];input[i].shape=(f+i)&1;
                for(unsigned j=0;j<ETAAC_SAMPLES;j++) {
                    int v=(int)(rng(&seed)&0xffffffu)-0x800000;
                    input[i].coeff[j]=(f%11==0)?0.0f:(float)v*0.125f;
                }
            }
        }
        double c1=now();
        for(unsigned i=0;i<count;i++) {
            float *g=gold+i*ETAAC_SAMPLES;
            if(operation==ETAAC_COPY) memcpy(g,input[i].coeff,ETAAC_SAMPLES*sizeof(float));
            else if(etaac_synth(g,expected[i].saved,input[i].coeff,input[i].sequence,
                               expected[i].sequence,input[i].shape,expected[i].shape,scratch)) {rc=-1;goto done;}
            if(capture.contexts && operation==ETAAC_SYNTH) {
                const ETAACRecord *r=capture_record(&capture,i,f);
                if(memcmp(g,r->output,sizeof(r->output)) || memcmp(expected[i].saved,r->after,sizeof(r->after))) {
                    fprintf(stderr,"CPU extraction != captured FFmpeg: frame%u slot%u\n",f,i);rc=-1;goto done;
                }
            }
            expected[i].generation=p.generation;expected[i].stream_id=i+1;
            expected[i].sequence=input[i].sequence;expected[i].shape=input[i].shape;expected[i].initialized=1;
        }
        double c2=now();
        RT(ff_et_runtime_write(rt,p.input_addr,input,p.input_bytes));double t1=now();
        RT(etaac_rt_launch(rt,&p,1));double t2=now();
        RT(ff_et_runtime_read(rt,output,p.output_addr,p.output_bytes));
        RT(ff_et_runtime_read(rt,status,p.status_addr,p.status_bytes));double t3=now();
        RT(ff_et_runtime_read(rt,states,p.state_addr,p.state_bytes));double t4=now();
        for(unsigned i=0;i<count;i++) {
            if(status[i].generation!=p.generation || status[i].stream_id!=i+1 ||
               status[i].result || status[i].samples!=ETAAC_SAMPLES) {
                fprintf(stderr,"bad completion frame%u slot%u rc%d generation%"PRIu64"\n",f,i,status[i].result,status[i].generation);rc=-1;goto done;
            }
        }
        if(memcmp(output,gold,p.output_bytes) || memcmp(states,expected,p.state_bytes)) {
            for(size_t i=0;i<p.output_bytes/sizeof(float);i++) if(memcmp(output+i,gold+i,sizeof(float))) {
                uint32_t a,b;memcpy(&a,output+i,4);memcpy(&b,gold+i,4);
                fprintf(stderr,"mismatch frame%u sample%zu actual%08x expected%08x\n",f,i,a,b);break;
            }
            fprintf(stderr,"exact PCM/state mismatch\n");rc=-1;goto done;
        }
        double t5=now();
        printf("{\"type\":\"batch\",\"generation\":%"PRIu64",\"pack_s\":%.9f,\"cpu_scalar_control_s\":%.9f,\"upload_s\":%.9f,\"launch_s\":%.9f,\"read_pcm_status_s\":%.9f,\"read_state_validation_s\":%.9f,\"compare_s\":%.9f,\"service_s\":%.9f,\"exact\":true}\n",p.generation,c1-c0,c2-c1,t1-c2,t2-t1,t3-t2,t4-t3,t5-t4,(c1-c0)+(t3-c2));
    }
    printf("{\"type\":\"result\",\"exact_channel_frames\":%u,\"pass\":true}\n",count*frames);ok=1;
done:
    ff_et_runtime_close(&rt);
    free(input);free(states);free(expected);free(status);free(output);free(gold);free(scratch);
    capture_free(&capture);
    return ok?0:1;
}
