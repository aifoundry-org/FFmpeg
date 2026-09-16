/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "protocol.h"
#include "dsp/synth.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
static void *z(size_t n) {void *p=NULL;assert(!posix_memalign(&p,64,n+64));memset(p,0,n);memset((char*)p+n,0xa5,64);return p;}
static void guard(void *p,size_t n) {for(unsigned i=0;i<64;i++)assert(((unsigned char*)p)[n+i]==0xa5);}
int main(void) {
    unsigned counts[]={1,8,32,64},harts[]={1,32,64};unsigned frames=0;
    for(unsigned a=0;a<4;a++) for(unsigned b=0;b<3;b++) {
        ETAACParams p={0};p.abi_version=ETAAC_ABI;p.count=counts[a];p.active_harts=harts[b];p.generation=1;
        p.input_bytes=p.count*sizeof(ETAACInput);p.state_bytes=p.count*sizeof(ETAACState);
        p.output_bytes=p.count*1024*sizeof(float);p.status_bytes=p.count*sizeof(ETAACStatus);
        p.scratch_bytes=p.active_harts*ETAAC_SCRATCH_FLOATS*sizeof(float);
        ETAACInput *in=z(p.input_bytes);ETAACState *state=z(p.state_bytes),*ref=z(p.state_bytes);
        float *out=z(p.output_bytes),*expected=z(p.output_bytes),*scratch=z(p.scratch_bytes),*cpu=z(4096*sizeof(float));
        ETAACStatus *st=z(p.status_bytes);
        p.input_addr=(uintptr_t)in;p.state_addr=(uintptr_t)state;p.output_addr=(uintptr_t)out;
        p.scratch_addr=(uintptr_t)scratch;p.status_addr=(uintptr_t)st;
        assert(etaac_params_valid(&p));
        for(unsigned f=0;f<12;f++) {
            p.generation=f+1;
            for(unsigned i=0;i<p.count;i++) {
                in[i].stream_id=i+100;in[i].expected_generation=f;in[i].sequence=f%4;in[i].shape=(i+f)%2;
                for(unsigned j=0;j<1024;j++) in[i].coeff[j]=(float)((int)((j*719+i*23+f*373)%65536)-32768);
                assert(!etaac_synth(expected+1024*i,ref[i].saved,in[i].coeff,in[i].sequence,ref[i].sequence,in[i].shape,ref[i].shape,cpu));
                ref[i].stream_id=in[i].stream_id;ref[i].generation=p.generation;ref[i].sequence=in[i].sequence;ref[i].shape=in[i].shape;ref[i].initialized=1;
            }
            for(unsigned h=0;h<64;h++) assert(!etaac_process(&p,h));
            assert(!memcmp(out,expected,p.output_bytes));assert(!memcmp(state,ref,p.state_bytes));
            for(unsigned i=0;i<p.count;i++) assert(st[i].generation==p.generation && st[i].result==0 && st[i].samples==1024);
            frames+=p.count;
        }
        ETAACParams bad=p;bad.status_addr=bad.input_addr;assert(!etaac_params_valid(&bad));
        bad=p;bad.scratch_bytes-=64;assert(!etaac_params_valid(&bad));
        bad=p;bad.input_addr=UINT64_MAX-63;assert(!etaac_params_valid(&bad));
        bad=p;bad.reserved[1]=1;assert(etaac_process(&bad,0)==ETAAC_BAD_PARAMS);
        bad=p;bad.active_harts=0;assert(!etaac_params_valid(&bad));
        bad=p;bad.count=65;assert(!etaac_params_valid(&bad));
        /* Stale work must not mutate the slot or publish success. */
        assert(etaac_process(&p,0)==ETAAC_BAD_STATE);
        assert(!memcmp(state,ref,p.state_bytes));assert(st[0].result && st[0].samples==0);
        in[0].expected_generation=12;p.generation=13;in[0].sequence=4;
        assert(etaac_process(&p,0)==ETAAC_BAD_INPUT);assert(!memcmp(state,ref,p.state_bytes));
        in[0].sequence=0;in[0].reserved[0]=1;assert(etaac_process(&p,0)==ETAAC_BAD_INPUT);
        in[0].reserved[0]=0;uint32_t nan=0x7fc00000;memcpy(in[0].coeff,&nan,4);
        assert(etaac_process(&p,0)==ETAAC_BAD_INPUT);assert(!memcmp(state,ref,p.state_bytes));
        guard(in,p.input_bytes);guard(state,p.state_bytes);guard(out,p.output_bytes);guard(st,p.status_bytes);guard(scratch,p.scratch_bytes);
        free(in);free(state);free(ref);free(out);free(expected);free(scratch);free(cpu);free(st);
    }
    /* Exact-end coefficient access adjacent to a protected page. */
    size_t pg=(size_t)sysconf(_SC_PAGESIZE);assert(pg>=4096);
    int fd=open("/dev/zero",0);assert(fd>=0);
    char *map=mmap(NULL,pg*2,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);close(fd);assert(map!=MAP_FAILED);
    assert(!mprotect(map+pg,pg,PROT_NONE));
    float *coeff=(float *)(map+pg-4096),*out=z(4096),*saved=z(2048),*scratch=z(16384);
    memset(coeff,0,4096);for(unsigned n=0;n<4;n++)assert(!etaac_synth(out,saved,coeff,n,0,0,1,scratch));
    munmap(map,pg*2);free(out);free(saved);free(scratch);
    printf("PASS native: %u exact channel frames; 1/32/64-hart ownership, invalid state/params, canaries, guarded input\n",frames);
    return 0;
}
