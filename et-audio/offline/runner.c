/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include "../capture_io.h"
#include <errno.h>
#include <inttypes.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static void *zero(size_t n){void *p=NULL;if(posix_memalign(&p,64,n))return NULL;memset(p,0,n);return p;}
static int number(const char *s,unsigned *v){char *e;errno=0;unsigned long n=strtoul(s,&e,10);if(errno||!*s||*e||n>1000000)return -1;*v=(unsigned)n;return 0;}
#define RT(call) do{if((rc=(call))<0){fprintf(stderr,"%s: %s (%d)\n",#call,ff_et_runtime_error(rt),rc);goto done;}}while(0)
int main(int argc,char **argv){
    unsigned count,harts,total,chunk,mode,op;
    if(argc!=9||number(argv[3],&count)||number(argv[4],&harts)||number(argv[5],&total)||
       number(argv[6],&chunk)||number(argv[7],&mode)||number(argv[8],&op)||!count||count>64||
       (harts!=1&&harts!=32&&harts!=64)||!total||total>512||!chunk||chunk>total||total%chunk||mode>1||op>1){
        fprintf(stderr,"usage: offline-run ELF CAPTURE CHANNELS HARTS FRAMES CHUNK MODE(0=streamed,1=resident) OP(0=synth,1=copy)\n");return 2;
    }
    if(!getenv("ETAAC_HW_GUARD")||strcmp(getenv("ETAAC_HW_GUARD"),"1")||
       !getenv("FF_ET_SYSEMU")||strcmp(getenv("FF_ET_SYSEMU"),"0")){
        fprintf(stderr,"Run only through offline/run-silicon.sh\n");return 2;
    }
    ETRuntime *rt=NULL;ETAACCapture c={0};int rc=0,ok=0;char error[512];
    ETAACInput *input=NULL;ETAACState *state=NULL;ETAACStatus *status=NULL;float *output=NULL;
    if(capture_load(&c,argv[2])||capture_select(&c,total)){fprintf(stderr,"invalid/short capture\n");goto done;}
    size_t in_bytes=(size_t)count*total*sizeof(*input),out_bytes=(size_t)count*total*1024*sizeof(float);
    input=zero(in_bytes);output=zero(out_bytes);state=zero(count*sizeof(*state));status=zero(count*sizeof(*status));
    if(!input||!output||!state||!status)goto done;
    double t=now();
    for(unsigned f=0;f<total;f++)for(unsigned i=0;i<count;i++){
        const ETAACRecord *r=capture_record(&c,i,f);ETAACInput *in=input+(size_t)f*count+i;
        in->stream_id=i+1;in->sequence=r->sequence;in->shape=r->shape;in->expected_generation=f;
        memcpy(in->coeff,r->coeff,sizeof(in->coeff));
        if(!f){state[i].initialized=1;state[i].stream_id=i+1;state[i].sequence=r->previous_sequence;state[i].shape=r->previous_shape;memcpy(state[i].saved,r->before,sizeof(state[i].saved));}
    }
    double pack=now()-t;
    ETAACOfflineParams p={0};p.abi_version=ETAAC_OFFLINE_ABI;p.count=count;p.active_harts=harts;p.operation=op;
    p.capacity_frames=mode?total:chunk;p.frames=chunk;p.generation=1;
    p.input_bytes=(uint64_t)count*p.capacity_frames*sizeof(ETAACInput);
    p.output_bytes=(uint64_t)count*p.capacity_frames*1024*sizeof(float);
    p.state_bytes=count*sizeof(ETAACState);p.status_bytes=count*sizeof(ETAACStatus);
    p.scratch_bytes=harts*ETAAC_SCRATCH_FLOATS*sizeof(float);
    t=now();rc=ff_et_runtime_open(&rt,argv[1],error,sizeof(error));
    if(rc<0){fprintf(stderr,"open: %s\n",error);goto done;}
    RT(ff_et_runtime_alloc(rt,p.input_bytes,&p.input_addr));RT(ff_et_runtime_alloc(rt,p.output_bytes,&p.output_addr));
    RT(ff_et_runtime_alloc(rt,p.state_bytes,&p.state_addr));RT(ff_et_runtime_alloc(rt,p.status_bytes,&p.status_addr));
    RT(ff_et_runtime_alloc(rt,p.scratch_bytes,&p.scratch_addr));
    RT(ff_et_runtime_write(rt,p.state_addr,state,p.state_bytes));double setup=now()-t;
    printf("{\"type\":\"config\",\"backend\":\"silicon-shire0\",\"channels\":%u,\"harts\":%u,\"frames\":%u,\"chunk\":%u,\"mode\":%u,\"operation\":%u,\"capture_contexts\":%u,\"device_bytes\":%"PRIu64",\"pack_s\":%.9f,\"setup_s\":%.9f}\n",count,harts,total,chunk,mode,op,c.selected_count,p.input_bytes+p.output_bytes+p.state_bytes+p.status_bytes+p.scratch_bytes,pack,setup);
    double upload=0,launch=0,completion=0,download=0;
    if(mode){t=now();RT(ff_et_runtime_write(rt,p.input_addr,input,in_bytes));upload+=now()-t;}
    for(unsigned start=0;start<total;start+=chunk){
        p.frame_offset=mode?start:0;p.generation=start+1;
        double u0=now();
        if(!mode)RT(ff_et_runtime_write(rt,p.input_addr,input+(size_t)start*count,p.input_bytes));
        double u1=now();
        RT(etaac_offline_launch(rt,&p,1));double u2=now();
        RT(ff_et_runtime_read(rt,status,p.status_addr,p.status_bytes));
        for(unsigned i=0;i<count;i++)if(status[i].generation!=start+chunk||status[i].stream_id!=i+1||status[i].result||status[i].samples!=chunk*1024){
            fprintf(stderr,"bad completion slot%u start%u rc%d samples%u gen%"PRIu64"\n",i,start,status[i].result,status[i].samples,status[i].generation);goto done;
        }
        double u3=now();
        if(!mode)RT(ff_et_runtime_read(rt,output+(size_t)start*count*1024,p.output_addr,p.output_bytes));
        double u4=now();
        upload+=u1-u0;launch+=u2-u1;completion+=u3-u2;download+=u4-u3;
        printf("{\"type\":\"launch\",\"start\":%u,\"frames\":%u,\"upload_s\":%.9f,\"launch_s\":%.9f,\"completion_s\":%.9f,\"download_s\":%.9f}\n",start,chunk,u1-u0,u2-u1,u3-u2,u4-u3);
    }
    if(mode){t=now();RT(ff_et_runtime_read(rt,output,p.output_addr,out_bytes));download+=now()-t;}
    t=now();RT(ff_et_runtime_read(rt,state,p.state_addr,p.state_bytes));double state_read=now()-t;t=now();
    for(unsigned f=0;f<total;f++)for(unsigned i=0;i<count;i++){
        const ETAACRecord *r=capture_record(&c,i,f);float *actual=output+((size_t)f*count+i)*1024;
        const float *expected=op?r->coeff:r->output;
        if(memcmp(actual,expected,1024*sizeof(float))){fprintf(stderr,"PCM mismatch frame%u slot%u\n",f,i);goto done;}
        if(f+1==total){
            const float *saved=op?capture_record(&c,i,0)->before:r->after;
            if(memcmp(state[i].saved,saved,512*sizeof(float))||state[i].generation!=total||state[i].stream_id!=i+1||state[i].sequence!=r->sequence||state[i].shape!=r->shape||state[i].initialized!=1){fprintf(stderr,"state mismatch slot%u\n",i);goto done;}
        }
    }
    double compare=now()-t;
    printf("{\"type\":\"result\",\"pass\":true,\"exact_channel_frames\":%u,\"pack_s\":%.9f,\"setup_s\":%.9f,\"upload_s\":%.9f,\"launch_s\":%.9f,\"completion_s\":%.9f,\"download_s\":%.9f,\"state_validation_s\":%.9f,\"compare_s\":%.9f,\"resident_phase_s\":%.9f,\"stage_service_s\":%.9f,\"cold_stage_s\":%.9f}\n",count*total,pack,setup,upload,launch,completion,download,state_read,compare,launch+completion,pack+upload+launch+completion+download,setup+pack+upload+launch+completion+download);
    ok=1;
done:
    ff_et_runtime_close(&rt);capture_free(&c);free(input);free(output);free(state);free(status);return ok?0:1;
}
