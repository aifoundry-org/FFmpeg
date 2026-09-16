/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Native lifecycle integration for the real AVCodec path, split requests,
 * scalar PCM/meter oracle, and optional post-INIT descriptor mutation. */
#include "protocol.h"
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libavutil/cpu.h>
#define INPUT_MAX (64u*1024u*1024u)
static uint8_t input[INPUT_MAX] __attribute__((aligned(64)));
static uint8_t state[ETAAC_FULL_STATE_BYTES] __attribute__((aligned(64)));
static uint8_t scratch[ETAAC_FULL_HEAP_BYTES+ETAAC_FULL_STACK_BYTES] __attribute__((aligned(64)));
static float output[ETAAC_FULL_MAX_PACKETS*2*1024] __attribute__((aligned(64)));
static float expected[ETAAC_FULL_MAX_PACKETS*2*1024] __attribute__((aligned(64)));
static ETAACFullStatus status __attribute__((aligned(64)));
static int read_exact(const char *name, void *dst, size_t want) {
    int fd=open(name,O_RDONLY); size_t got=0; if(fd<0)return -1;
    while(got<want) { ssize_t n=read(fd,(uint8_t *)dst+got,want-got); if(n<=0){close(fd);return -1;} got+=(size_t)n; }
    return close(fd) ? -1 : 0;
}
static int file_size(const char *name, size_t *n) { struct stat st; if(stat(name,&st)||st.st_size<0||(uintmax_t)st.st_size>INPUT_MAX)return -1;*n=(size_t)st.st_size;return 0; }
static ETAACFullParams params(size_t inbytes, uint32_t cap, uint32_t channels, uint32_t op, uint64_t gen, uint32_t first, uint32_t packets) {
    ETAACFullParams p; memset(&p,0,sizeof(p)); p.abi_version=ETAAC_FULL_ABI;p.operation=op;p.count=1;
    p.input_addr=(uintptr_t)input;p.input_bytes=inbytes;p.state_addr=(uintptr_t)state;p.state_bytes=sizeof(state);
    p.output_addr=(uintptr_t)output;p.output_bytes=(uint64_t)cap*channels*1024*sizeof(float);
    p.scratch_addr=(uintptr_t)scratch;p.scratch_bytes=sizeof(scratch);p.status_addr=(uintptr_t)&status;p.status_bytes=sizeof(status);
    p.generation=gen;p.first_packet=first;p.packets=packets;p.capacity_frames=cap;return p;
}
int main(int argc,char **argv) {
    size_t inbytes, pcmbytes; ETAACFullInput *h; ETAACFullParams p;
    if((argc!=3 && argc!=4) || file_size(argv[1],&inbytes) || read_exact(argv[1],input,inbytes))return 2;
    h=(ETAACFullInput *)input;
    if(!etaac_full_input_valid(input,inbytes,h->packet_count) )return 3;
    pcmbytes=(size_t)h->packet_count*h->channels*1024*sizeof(float);
    if(read_exact(argv[2],expected,pcmbytes))return 4;
    av_force_cpu_flags(0);
    p=params(inbytes,h->packet_count,h->channels,ETAAC_FULL_INIT,1,0,0);
    if(etaac_full_process(&p)||status.result||status.frames||status.samples)return 5;
    if(argc==4) {
        ETAACFullPacket *d=(ETAACFullPacket *)(input+sizeof(*h));
        if(!strcmp(argv[3],"padding"))input[d[0].offset+d[0].bytes]=1;
        else if(!strcmp(argv[3],"length"))d[0].bytes=UINT32_MAX;
        else return 10;
        memset(output,0x6e,pcmbytes);
        p=params(inbytes,h->packet_count,h->channels,ETAAC_FULL_DECODE,1,0,1);
        if(etaac_full_process(&p)>=0 || status.result>=0 || status.frames)return 11;
        for(size_t i=0;i<pcmbytes;i++)if(((uint8_t *)output)[i]!=0x6e)return 12;
        /* Failed state cannot be reused or passed back to FFmpeg close. */
        p=params(inbytes,h->packet_count,h->channels,ETAAC_FULL_CLOSE,h->packet_count+1,0,0);
        return etaac_full_process(&p)<0 ? 0 : 13;
    }
    /* Deliberately split into one-packet requests to exercise persistent state. */
    for(unsigned i=0;i<h->packet_count;i++) {
        p=params(inbytes,h->packet_count,h->channels,ETAAC_FULL_DECODE,i+1,i,1);
        if(etaac_full_process(&p)||status.result||status.frames!=1||status.samples!=1024||status.heap_failures||status.unsupported_calls)return 6;
    }
    if(memcmp(output,expected,pcmbytes))return 9;
    uint64_t peak=0,above=0;
    for(size_t i=0;i<pcmbytes/4;i++){uint32_t u;memcpy(&u,expected+i,4);u&=UINT32_C(0x7fffffff);if(u>peak)peak=u;if(u>UINT32_C(0x3f800000))above++;}
    p=params(inbytes,h->packet_count,h->channels,ETAAC_FULL_METER,1,0,h->packet_count);
    if(etaac_full_process(&p)||status.result||!status.heap_peak||status.consumer_peak_bits!=peak||status.consumer_above_one!=above)return 7;
    p=params(inbytes,h->packet_count,h->channels,ETAAC_FULL_CLOSE,h->packet_count+1,0,0);
    if(etaac_full_process(&p)||status.result||status.heap_live)return 8;
    return 0;
}
