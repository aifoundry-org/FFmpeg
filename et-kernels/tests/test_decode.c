/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "et_mpeg2.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void put(uint8_t *p, unsigned *pos, unsigned value, unsigned n)
{
    for (unsigned i=n; i; i--) {
        if ((value >> (i-1)) & 1) p[*pos/8] |= 1 << (7-*pos%8);
        (*pos)++;
    }
}
static unsigned gray_slice(uint8_t *p, unsigned row, unsigned mbs)
{
    memset(p,0,128); p[2]=1; p[3]=row+1;
    unsigned pos=32;
    put(p,&pos,8,5); put(p,&pos,0,1);
    for (unsigned x=0; x<mbs; x++) {
        put(p,&pos,1,1); put(p,&pos,1,1);
        for (int n=0; n<6; n++) {
            put(p,&pos,n<4 ? 4:0,n<4 ? 3:2);
            put(p,&pos,2,2);
        }
    }
    return (pos+7)/8;
}
int main(void)
{
    enum { ROWS=65, INPUT=16384, FRAME=ROWS*32*64 };
    uint8_t *input=aligned_alloc(64,INPUT), *output=aligned_alloc(64,FRAME+64);
    ETSliceStatus *status=aligned_alloc(64,ROWS*64);
    assert(input && output && status);
    memset(input,0,INPUT); memset(output,0xa5,FRAME+64); memset(status,0xcc,ROWS*64);
    ETFrameParams p={0};
    p.input_addr=(uintptr_t)input; p.dst_addr=(uintptr_t)output;
    p.status_addr=(uintptr_t)status; p.abi_version=ET_MPEG2_ABI_VERSION;
    p.nb_slices=p.mb_height=ROWS; p.mb_width=2; p.width=32; p.height=ROWS*16;
    p.linesize_y=p.linesize_uv=64; p.quant_offset=0; p.slice_table_offset=512;
    p.bitstream_offset=2048; p.frame_bytes=FRAME; p.input_bytes=INPUT;
    p.plane_offset[1]=ROWS*16*64; p.plane_offset[2]=ROWS*24*64;
    p.pict_type=1; p.picture_structure=3; p.frame_pred_frame_dct=p.progressive_frame=1;
    p.active_harts=64; p.frame_id=42;
    uint16_t *matrix=(uint16_t *)input;
    for (int i=0;i<256;i++) matrix[i]=16;
    ETSliceDesc *s=(ETSliceDesc *)(input+512);
    for (unsigned i=0;i<ROWS;i++) {
        s[i].bitstream_off=i*128; s[i].mb_y=i;
        s[i].bitstream_len=gray_slice(input+2048+i*128,i,2);
    }
    assert(et_mpeg2_decode(&p,64)==0);
    assert(status[0].code==0xcccccccc);
    /* Reverse hart order, hart zero must own rows 0 and 64. */
    for (int h=63;h>=0;h--) assert(et_mpeg2_decode(&p,h)==0);
    for (unsigned i=0;i<ROWS;i++) {
        assert(status[i].code==0 && status[i].mb_decoded==2 && status[i].frame_id==42);
    }
    for (unsigned plane=0;plane<3;plane++)
        for (unsigned y=0;y<ROWS*(plane?8:16);y++)
            for (unsigned x=0;x<64;x++)
                assert(output[p.plane_offset[plane]+y*64+x]==(x<(plane?16:32)?128:0xa5));
    for (unsigned i=FRAME;i<FRAME+64;i++) assert(output[i]==0xa5);
    p.active_harts=1;
    assert(et_mpeg2_decode(&p,0)==0);
    unsigned len=s[0].bitstream_len;
    for (unsigned n=0;n<len;n++) {
        s[0].bitstream_len=n;
        assert(et_mpeg2_decode(&p,0)!=0);
        assert(status[0].code!=0);
    }
    s[0].bitstream_len=gray_slice(input+2048,0,1); /* partial row */
    assert(et_mpeg2_decode(&p,0)==ET_DECODE_BAD_SLICE);
    s[0].bitstream_len=gray_slice(input+2048,0,3); /* row overrun */
    assert(et_mpeg2_decode(&p,0)==ET_DECODE_ROW_OVERFLOW);
    s[0].bitstream_len=gray_slice(input+2048,0,2);
    s[1].mb_y=0;
    assert(et_mpeg2_decode(&p,0)==ET_DECODE_BAD_PARAMS);
    s[1].mb_y=1;
    p.pict_type=4;
    assert(et_mpeg2_decode(&p,0)==ET_DECODE_UNSUPPORTED);
    p.pict_type=1;
    /* Invalid launch controls still publish a fresh generation, without a
     * per-frame status memset. Exactly fallback logical hart zero writes. */
    p.frame_id++;
    p.active_harts=0;
    assert(et_mpeg2_decode(&p,1)==ET_DECODE_BAD_PARAMS);
    assert(status[0].frame_id!=p.frame_id);
    assert(et_mpeg2_decode(&p,0)==ET_DECODE_BAD_PARAMS);
    for (unsigned i=0;i<ROWS;i++)
        assert(status[i].code==ET_DECODE_BAD_PARAMS && status[i].frame_id==p.frame_id && !status[i].mb_decoded);
    p.active_harts=1;
    p.abi_version++;
    p.frame_id++;
    assert(et_mpeg2_decode(&p,0)==ET_DECODE_BAD_PARAMS);
    for (unsigned i=0;i<ROWS;i++) assert(status[i].frame_id==p.frame_id);
    p.abi_version--;
    p.frame_id++;
    assert(et_mpeg2_decode(&p,0)==0);
    for (unsigned i=0;i<ROWS;i++) assert(status[i].frame_id==p.frame_id && status[i].code==0);
    /* All single-bit corruptions must terminate without touching the canary. */
    for (unsigned bit=32;bit<len*8;bit++) {
        input[2048+bit/8]^=1<<(7-bit%8);
        (void)et_mpeg2_decode(&p,0);
        input[2048+bit/8]^=1<<(7-bit%8);
        for (unsigned i=FRAME;i<FRAME+64;i++) assert(output[i]==0xa5);
    }
    /* End the unpadded input allocation immediately before a guard page.
     * Zero-padding must be supplied by bit_window, not by caller memory. */
    long pagesize=sysconf(_SC_PAGESIZE);
    assert(pagesize>=4096);
    uint8_t *guard=mmap(NULL,2*pagesize,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(guard!=MAP_FAILED && !mprotect(guard+pagesize,pagesize,PROT_NONE));
    ETFrameParams one=p;
    one.nb_slices=one.mb_height=1; one.height=16;
    one.bitstream_offset=576; one.input_bytes=576+len;
    /* Input base needs only eight-byte alignment, not cache-line alignment.
     * Tail length rounded to 8 bytes leaves at most seven legal zero bytes. */
    one.input_bytes=(one.input_bytes+7)&~7u;
    uint8_t *guard_input=guard+pagesize-one.input_bytes;
    one.input_addr=(uintptr_t)guard_input;
    memcpy(guard_input,input,576);
    ETSliceDesc *gs=(ETSliceDesc *)(guard_input+512);
    gs->bitstream_off=0; gs->bitstream_len=one.input_bytes-576; gs->mb_y=0;
    memset(guard_input+576,0,gs->bitstream_len);
    uint8_t temp[128]; gray_slice(temp,0,2);
    memcpy(guard_input+576,temp,len);
    assert(et_mpeg2_decode(&one,0)==0);
    unsigned seed=0x13579bdf;
    for (unsigned attempt=0;attempt<3000;attempt++) {
        for (unsigned j=4;j<gs->bitstream_len;j++) {
            seed=1664525u*seed+1013904223u;
            guard_input[576+j]=seed>>24;
        }
        (void)et_mpeg2_decode(&one,0);
    }
    /* Inter-picture malformed reads use the same guard-page window, with
     * distinct resident references. Vector/codeword corruption must never
     * reach outside a reference allocation or destination macroblock row. */
    uint8_t *forward=aligned_alloc(64,FRAME), *backward=aligned_alloc(64,FRAME);
    assert(forward && backward);
    memset(forward,128,FRAME); memset(backward,129,FRAME);
    one.ref_fwd_addr=(uintptr_t)forward; one.ref_bwd_addr=(uintptr_t)backward;
    for (unsigned direction=0;direction<2;direction++)
        for (unsigned axis=0;axis<2;axis++) one.f_code[direction][axis]=1;
    for (unsigned type=2;type<=3;type++) {
        one.pict_type=type;
        for (unsigned attempt=0;attempt<3000;attempt++) {
            for (unsigned j=4;j<gs->bitstream_len;j++) {
                seed=1664525u*seed+1013904223u;
                guard_input[576+j]=seed>>24;
            }
            (void)et_mpeg2_decode(&one,0);
        }
    }
    one.f_code[0][0]=0;
    assert(et_mpeg2_decode(&one,0)==ET_DECODE_UNSUPPORTED);
    one.f_code[0][0]=1;
    one.ref_fwd_addr=one.dst_addr;
    assert(et_mpeg2_decode(&one,0)==ET_DECODE_BAD_PARAMS);
    free(forward); free(backward);
    assert(!munmap(guard,2*pagesize));
    free(status); free(output); free(input);
    puts("ET MPEG2: native gray I, 1/64 harts, row ownership, malformed/truncation tests passed");
    return 0;
}
