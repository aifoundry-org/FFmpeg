/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#ifdef ETAAC_FULL_EMULATOR
#define FULL_GUARD "ETAAC_FULL_EMULATOR_GUARD"
#define FULL_SYSEMU "1"
#define FULL_BACKEND "sys_emu-shire0-hart0"
#else
#define FULL_GUARD "ETAAC_FULL_HW_GUARD"
#define FULL_SYSEMU "0"
#define FULL_BACKEND "silicon-shire0-hart0"
#endif
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static void *zero(size_t n){void *p=0;if(posix_memalign(&p,64,n))return 0;memset(p,0,n);return p;}
static void *load(const char *name,size_t *n,size_t max){FILE *f=fopen(name,"rb");void *p=0;long z;if(!f)return 0;if(fseek(f,0,SEEK_END)||(z=ftell(f))<=0||(uint64_t)z>max||fseek(f,0,SEEK_SET))goto done;*n=(size_t)z;p=zero((*n+63)&~(size_t)63);if(p&&fread(p,1,*n,f)!=*n){free(p);p=0;}done:fclose(f);return p;}
static int integer(const char *s,unsigned *v){char *e;errno=0;unsigned long n=strtoul(s,&e,10);if(errno||!*s||*e||n>2048)return -1;*v=(unsigned)n;return 0;}
static void status_json(const ETAACFullStatus *s,double launch,double read){
 printf("{\"type\":\"status\",\"operation\":%u,\"generation\":%"PRIu64",\"result\":%d,\"frames\":%"PRIu64",\"samples_per_channel\":%"PRIu64",\"heap_peak\":%"PRIu64",\"init_ticks\":%"PRIu64",\"decode_ticks\":%"PRIu64",\"publication_ticks\":%"PRIu64",\"unsupported_calls\":%"PRIu64",\"heap_failures\":%"PRIu64",\"consumer_peak_bits\":%"PRIu64",\"consumer_above_one\":%"PRIu64",\"launch_s\":%.9f,\"status_io_s\":%.9f}\n",s->operation,s->generation,s->result,s->frames,s->samples,s->heap_peak,s->init_ticks,s->decode_ticks,s->pcm_publish_ticks,s->unsupported_calls,s->heap_failures,s->consumer_peak_bits,s->consumer_above_one,launch,read);
 printf("{\"type\":\"context_publication\",\"operation\":%u,\"generation\":%"PRIu64",\"ticks\":%"PRIu64",\"heap_live\":%"PRIu64",\"stack_guard_ok\":%"PRIu64"}\n",s->operation,s->generation,s->context_publish_ticks,s->heap_live,s->stack_guard_ok);
 fflush(stdout);
}
static int dispatch(ETRuntime *rt,ETAACFullParams *p,ETAACFullStatus *s,uint64_t frames,double *launch,double *read){
 /* Fresh failure sentinel prevents an unwritten/stale completion from passing. */
 ETAACFullStatus poison={0};poison.result=-999;
 double t=now();int rc=ff_et_runtime_write(rt,p->status_addr,&poison,sizeof(poison));*read=now()-t;
 if(rc<0){fprintf(stderr,"poison status: %s\n",ff_et_runtime_error(rt));return -1;}
 t=now();rc=etaac_full_launch(rt,p,1);*launch=now()-t;
 if(rc<0){fprintf(stderr,"launch: %s\n",ff_et_runtime_error(rt));return -1;}
 t=now();rc=ff_et_runtime_read(rt,s,p->status_addr,sizeof(*s));*read+=now()-t;
 if(rc<0){fprintf(stderr,"read status: %s\n",ff_et_runtime_error(rt));return -1;}
 status_json(s,*launch,*read);
 if(s->stack_guard_ok!=1||s->result||s->operation!=p->operation||s->generation!=p->generation||s->frames!=frames||s->samples!=frames*1024||s->unsupported_calls||s->heap_failures){fprintf(stderr,"invalid/failed completion op%u rc%d gen%"PRIu64" frames%"PRIu64"\n",p->operation,s->result,s->generation,s->frames);return -1;}return 0;
}
#define RT(expr) do{int r_=(expr);if(r_<0){fprintf(stderr,"%s: %s (%d)\n",#expr,ff_et_runtime_error(rt),r_);goto done;}}while(0)
int main(int argc,char **argv){
 unsigned chunk,meter;size_t in_n=0,gold_n=0;void *input=0,*gold=0,*pcm=0,*state=0;ETRuntime *rt=0;char error[512];int pass=0;uint64_t mismatches=0;
 if(argc!=8||integer(argv[4],&chunk)||!chunk||integer(argv[5],&meter)||meter>1){fprintf(stderr,"usage: full-run ELF INPUT GOLD_PCM CHUNK METER ACTUAL_PCM STATUS_STATE\n");return 2;}
 if(!getenv(FULL_GUARD)||strcmp(getenv(FULL_GUARD),"1")||!getenv("FF_ET_SYSEMU")||strcmp(getenv("FF_ET_SYSEMU"),FULL_SYSEMU)){fprintf(stderr,"use full/run-silicon.sh\n");return 2;}
 input=load(argv[2],&in_n,64u*1024u*1024u);if(!input||!etaac_full_input_valid(input,in_n,((ETAACFullInput *)input)->packet_count)){fprintf(stderr,"bad input envelope\n");goto done;}
 ETAACFullInput *h=input;if(chunk>h->packet_count||h->packet_count%chunk){fprintf(stderr,"chunk must divide packet count\n");goto done;}
 size_t out_n=(size_t)h->packet_count*h->channels*1024*sizeof(float);
 gold=load(argv[3],&gold_n,out_n);if(!gold||gold_n!=out_n){fprintf(stderr,"bad golden size\n");goto done;}
 for(size_t i=0;i<gold_n/4;i++){uint32_t b;memcpy(&b,(char *)gold+i*4,4);if((b&0x7f800000)==0x7f800000){fprintf(stderr,"nonfinite oracle\n");goto done;}}
 pcm=zero(out_n);state=zero(ETAAC_FULL_STATE_BYTES);if(!pcm||!state)goto done;
 ETAACFullStatus st={0};ETAACFullParams p={0};p.abi_version=ETAAC_FULL_ABI;p.count=1;p.capacity_frames=h->packet_count;p.generation=1;
 p.input_bytes=in_n;p.output_bytes=out_n;p.state_bytes=ETAAC_FULL_STATE_BYTES;p.scratch_bytes=ETAAC_FULL_HEAP_BYTES+ETAAC_FULL_STACK_BYTES;p.status_bytes=sizeof(st);
 double t=now();if(ff_et_runtime_open(&rt,argv[1],error,sizeof(error))<0){fprintf(stderr,"open: %s\n",error);goto done;}
 RT(ff_et_runtime_alloc(rt,p.input_bytes,&p.input_addr));RT(ff_et_runtime_alloc(rt,p.state_bytes,&p.state_addr));RT(ff_et_runtime_alloc(rt,p.output_bytes,&p.output_addr));RT(ff_et_runtime_alloc(rt,p.scratch_bytes,&p.scratch_addr));RT(ff_et_runtime_alloc(rt,p.status_bytes,&p.status_addr));
 RT(ff_et_runtime_write(rt,p.state_addr,state,p.state_bytes));double runtime_setup=now()-t;
 t=now();RT(ff_et_runtime_write(rt,p.input_addr,input,in_n));double upload=now()-t;
 printf("{\"type\":\"config\",\"backend\":\"" FULL_BACKEND "\",\"protocol\":4,\"packets\":%u,\"channels\":%u,\"sample_rate\":%u,\"chunk\":%u,\"meter\":%u,\"input_bytes\":%zu,\"output_bytes\":%zu,\"device_bytes\":%"PRIu64"}\n",h->packet_count,h->channels,h->sample_rate,chunk,meter,in_n,out_n,p.input_bytes+p.output_bytes+p.state_bytes+p.status_bytes+p.scratch_bytes);fflush(stdout);
 double init,init_read,decode=0,status_reads=0,close,close_read,consumer=0,consumer_read=0;
 p.operation=ETAAC_FULL_INIT;if(dispatch(rt,&p,&st,0,&init,&init_read))goto done;
 for(unsigned first=0;first<h->packet_count;first+=chunk){double a,b;p.operation=ETAAC_FULL_DECODE;p.first_packet=first;p.packets=chunk;p.generation=first+1;if(dispatch(rt,&p,&st,chunk,&a,&b))goto done;decode+=a;status_reads+=b;}
 if(meter){p.operation=ETAAC_FULL_METER;p.first_packet=0;p.packets=h->packet_count;p.generation=1;if(dispatch(rt,&p,&st,0,&consumer,&consumer_read))goto done;uint64_t peak=0,above=0;for(size_t i=0;i<out_n/4;i++){uint32_t b;memcpy(&b,(char *)gold+4*i,4);b&=0x7fffffff;if(b>peak)peak=b;if(b>0x3f800000)above++;}if(peak!=st.consumer_peak_bits||above!=st.consumer_above_one){fprintf(stderr,"meter mismatch\n");goto done;}}
 t=now();RT(ff_et_runtime_read(rt,pcm,p.output_addr,out_n));double download=now()-t;
 t=now();RT(ff_et_runtime_read(rt,state,p.state_addr,p.state_bytes));double state_read=now()-t;
 FILE *f=fopen(argv[6],"wbx");if(!f){perror("actual output");goto done;}int written=fwrite(pcm,1,out_n,f)==out_n;written&=!fclose(f);if(!written)goto done;
 f=fopen(argv[7],"wbx");if(!f)goto done;written=fwrite(state,1,p.state_bytes,f)==p.state_bytes;written&=!fclose(f);if(!written)goto done;
 t=now();for(size_t i=0;i<out_n/4;i++){uint32_t a,b;memcpy(&a,(char *)pcm+i*4,4);memcpy(&b,(char *)gold+i*4,4);if(a!=b){if(mismatches<12)fprintf(stderr,"PCM word%zu actual=%08x expected=%08x\n",i,a,b);mismatches++;}}double compare=now()-t;
 p.operation=ETAAC_FULL_CLOSE;p.first_packet=0;p.packets=0;p.generation=h->packet_count+1;if(dispatch(rt,&p,&st,0,&close,&close_read))goto done;
 pass=!mismatches;
 printf("{\"type\":\"result\",\"pass\":%s,\"exact_packet_frames\":%u,\"exact_channel_frames\":%u,\"pcm_mismatches\":%"PRIu64",\"runtime_setup_s\":%.9f,\"upload_s\":%.9f,\"decoder_init_s\":%.9f,\"init_status_io_s\":%.9f,\"decode_s\":%.9f,\"decode_status_io_s\":%.9f,\"consumer_s\":%.9f,\"consumer_status_io_s\":%.9f,\"pcm_readback_s\":%.9f,\"state_validation_read_s\":%.9f,\"compare_s\":%.9f,\"decoder_close_s\":%.9f,\"close_status_io_s\":%.9f,\"resident_service_s\":%.9f,\"transfer_inclusive_service_s\":%.9f}\n",pass?"true":"false",pass?h->packet_count:0,pass?h->packet_count*h->channels:0,mismatches,runtime_setup,upload,init,init_read,decode,status_reads,consumer,consumer_read,download,state_read,compare,close,close_read,decode+status_reads+consumer+consumer_read,upload+decode+status_reads+consumer+consumer_read+(meter?0:download));
 done:ff_et_runtime_close(&rt);free(input);free(gold);free(pcm);free(state);return pass?0:1;
}
