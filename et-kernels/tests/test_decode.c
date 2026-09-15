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
static unsigned gray_slice(uint8_t *p, size_t capacity, unsigned row, unsigned mbs)
{
    unsigned bytes = (38 + 30 * mbs + 7) / 8;
    assert(mbs && mbs <= 1024 && bytes <= capacity);
    memset(p,0,bytes); p[2]=1; p[3]=row+1;
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
/* Place the input in an aligned allocation whose last declared byte is at
 * most seven bytes before PROT_NONE. Offset 0..7 independently exercises every
 * slice-start alignment; for every length, one offset ends exactly at the page.
 * Nothing after the slice is part of input_bytes or supplied as ABI padding. */
static uint8_t *guard_slice(ETFrameParams *p, uint8_t *end,
                            const uint8_t *payload, unsigned len, unsigned off)
{
    p->input_bytes=576+off+len;
    uint8_t *input=end-((p->input_bytes+7)&~7u);
    assert(!((uintptr_t)input&7));
    p->input_addr=(uintptr_t)input;
    p->frame_id++;
    memset(input,0,576+off);
    uint16_t *matrix=(uint16_t *)input;
    for (unsigned i=0;i<256;i++) matrix[i]=16;
    ETSliceDesc *slice=(ETSliceDesc *)(input+512);
    slice->bitstream_off=off;
    slice->bitstream_len=len;
    memcpy(input+576+off,payload,len);
    return input+576+off;
}

static void check_bytes(const uint8_t *p, size_t bytes, unsigned value)
{
    for (size_t i=0;i<bytes;i++) assert(p[i]==value);
}

static void check_wide_output(const ETFrameParams *p, int pixels)
{
    const uint8_t *output=(const uint8_t *)(uintptr_t)p->dst_addr;
    check_bytes(output-64,64,0xa5);
    check_bytes(output+p->frame_bytes,64,0xa5);
    for (unsigned plane=0;plane<3;plane++) {
        unsigned stride=plane ? p->linesize_uv : p->linesize_y;
        unsigned width=p->mb_width*(plane ? 8 : 16);
        for (unsigned y=0;y<(plane ? 8u : 16u);y++) {
            const uint8_t *row=output+p->plane_offset[plane]+y*stride;
            if (pixels) check_bytes(row,width,128);
            check_bytes(row+width,stride-width,0xa5);
        }
    }
    const uint8_t *status=(const uint8_t *)(uintptr_t)p->status_addr;
    check_bytes(status-64,64,0xcc);
    check_bytes(status+64,64,0xcc);
}

static int decode_guarded(ETFrameParams *p, uint8_t *end,
                          const uint8_t *payload, unsigned len, unsigned off)
{
    uint8_t *slice=guard_slice(p,end,payload,len,off);
    ETSliceStatus *status=(ETSliceStatus *)(uintptr_t)p->status_addr;
    int ret=et_mpeg2_decode(p,0);
    assert(ret!=ET_DECODE_BAD_PARAMS && ret!=ET_DECODE_UNSUPPORTED);
    assert(status->code==(unsigned)ret && status->frame_id==p->frame_id);
    assert(status->mb_decoded<=p->mb_width && status->bits_consumed<=len*8);
    if (!ret) assert(status->mb_decoded==p->mb_width);
    assert(!memcmp(slice,payload,len));
    return ret;
}

static void test_long_guarded_slices(void)
{
    /* 256-byte block windows, 256+64 direct lookahead, the former 512-byte
     * cache, and current 1024-byte cache boundaries, plus multiple refills and
     * the maximum 1024-MB row. These are actual coded rows, not tiny slices
     * inflated entirely with zero stuffing. */
    static const unsigned lengths[]={
        255,256,257,319,320,321,323,324,325,326,511,512,513,
        1023,1024,1025,2047,2048,2049,3845
    };
    /* Also probe header (4-byte fast read / 8+64 VLC) and direct block
     * lookahead transitions with every slice-start alignment. */
    static const unsigned cuts[]={
        0,1,2,3,4,5,6,7,8,9,63,64,65,71,72,73,75,76,77,
        255,256,257,319,320,321,323,324,325,326,511,512,513,
        767,768,769,1023,1024,1025,2047,2048,2049
    };
    long page=sysconf(_SC_PAGESIZE);
    assert(page>=4096);
    size_t readable=((576+7+3845+page-1)/page)*page;
    uint8_t *mapping=mmap(NULL,readable+page,PROT_READ|PROT_WRITE,
                          MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(mapping!=MAP_FAILED);
    uint8_t *end=mapping+readable;
    assert(!mprotect(end,page,PROT_NONE));
    uint32_t seed=0x2468ace1;
    unsigned valid=0,truncated=0,random=0;
    for (unsigned c=0;c<sizeof(lengths)/sizeof(*lengths);c++) {
        unsigned len=lengths[c], mbs=(len*8-38)/30;
        uint8_t *payload=calloc(1,len), *damaged=malloc(len);
        assert(payload && damaged);
        unsigned coded=gray_slice(payload,len,0,mbs);
        ETFrameParams p={0};
        p.abi_version=ET_MPEG2_ABI_VERSION;
        p.nb_slices=p.mb_height=1; p.mb_width=mbs;
        p.width=mbs*16; p.height=16;
        /* An extra cache line of stride padding catches row overruns even
         * when the maximum-width fixture has no natural alignment slack. */
        p.linesize_y=((p.width+63)&~63u)+64;
        p.linesize_uv=((mbs*8+63)&~63u)+64;
        p.plane_offset[1]=16*p.linesize_y;
        p.plane_offset[2]=p.plane_offset[1]+8*p.linesize_uv;
        p.frame_bytes=p.plane_offset[2]+8*p.linesize_uv;
        p.slice_table_offset=512; p.bitstream_offset=576;
        p.pict_type=1; p.picture_structure=3;
        p.frame_pred_frame_dct=p.progressive_frame=1;
        p.active_harts=1; p.frame_id=1000;
        uint8_t *output=aligned_alloc(64,p.frame_bytes+128);
        uint8_t *status=aligned_alloc(64,192);
        uint8_t *forward=aligned_alloc(64,p.frame_bytes);
        uint8_t *backward=aligned_alloc(64,p.frame_bytes);
        assert(output && status && forward && backward);
        memset(output,0xa5,p.frame_bytes+128); memset(status,0xcc,192);
        memset(forward,128,p.frame_bytes); memset(backward,129,p.frame_bytes);
        p.dst_addr=(uintptr_t)(output+64); p.status_addr=(uintptr_t)(status+64);
        p.ref_fwd_addr=(uintptr_t)forward; p.ref_bwd_addr=(uintptr_t)backward;
        for (unsigned direction=0;direction<2;direction++)
            for (unsigned axis=0;axis<2;axis++) p.f_code[direction][axis]=1;
        for (unsigned off=0;off<8;off++) {
            assert(!decode_guarded(&p,end,payload,len,off));
            assert(((ETSliceStatus *)(status+64))->bits_consumed==38+30*mbs);
            check_wide_output(&p,1);
            valid++;
        }
        /* Relocate every truncated prefix next to PROT_NONE, rather than
         * only reducing a descriptor inside a still-readable full slice. */
        for (unsigned n=0;n<len;n++) {
            int ret=decode_guarded(&p,end,payload,n,n&7);
            assert(n<coded ? ret!=0 : ret==0);
            truncated++;
        }
        for (unsigned i=0;i<sizeof(cuts)/sizeof(*cuts);i++) {
            unsigned n=cuts[i];
            if (n>=len) continue;
            for (unsigned off=0;off<8;off++) {
                int ret=decode_guarded(&p,end,payload,n,off);
                assert(n<coded ? ret!=0 : ret==0);
                truncated++;
            }
        }
        /* Random full payloads cover all picture types. Keeping a valid I
         * prefix in half the I trials forces parsing deep into long windows,
         * rather than almost always failing at the first macroblock header. */
        for (unsigned type=1;type<=3;type++) {
            p.pict_type=type;
            for (unsigned attempt=0;attempt<128;attempt++) {
                memcpy(damaged,payload,len);
                unsigned begin=4;
                if (type==1 && (attempt&1)) begin=4+(attempt*37)%(coded-4);
                for (unsigned j=begin;j<len;j++) {
                    seed=1664525u*seed+1013904223u;
                    damaged[j]=seed>>24;
                }
                (void)decode_guarded(&p,end,damaged,len,attempt&7);
                random++;
            }
        }
        /* Failed decodes may touch pixels, never line padding or guards. */
        check_wide_output(&p,0);
        check_bytes(forward,p.frame_bytes,128);
        check_bytes(backward,p.frame_bytes,129);
        free(backward); free(forward); free(status); free(output);
        free(damaged); free(payload);
    }
    assert(!munmap(mapping,readable+page));
    printf("ET MPEG2: long guard-page slices: %u valid, %u truncation, %u random cases\n",
           valid,truncated,random);
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
        s[i].bitstream_len=gray_slice(input+2048+i*128,128,i,2);
    }
    assert(et_mpeg2_decode(&p,64)==0);
    check_bytes((const uint8_t *)status,ROWS*64,0xcc);
    check_bytes(output,FRAME+64,0xa5);
    /* Reverse order and use a distinct generation for every hart: detect
     * duplicate ownership even when a repeated decode writes identical pixels.
     * CMake keeps scheduling defines private to the library, so accept exactly
     * the three supported ownership maps, not an arbitrary row permutation. */
    unsigned owners[ROWS];
    for (unsigned i=0;i<ROWS;i++) owners[i]=64;
    for (int h=63;h>=0;h--) {
        ETSliceStatus before[ROWS];
        memcpy(before,status,sizeof(before));
        p.frame_id=100+h;
        assert(et_mpeg2_decode(&p,h)==0);
        for (unsigned i=0;i<ROWS;i++) {
            if (!memcmp(status+i,before+i,sizeof(*status))) continue;
            assert(owners[i]==64 && status[i].frame_id==p.frame_id);
            assert(status[i].code==0 && status[i].mb_decoded==2);
            owners[i]=h;
        }
    }
    int linear=1, spread=1, even=1;
    for (unsigned i=0;i<ROWS;i++) {
        unsigned worker=i%64;
        linear &= owners[i]==worker;
        spread &= owners[i]==2*(worker%32)+worker/32;
        even &= owners[i]==2*(i%32);
    }
    assert(linear || spread || even);
    p.frame_id=42;
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
    p.frame_id++;
    assert(et_mpeg2_decode(&p,1)==0);
    for (unsigned i=0;i<ROWS;i++) assert(status[i].frame_id==42);
    assert(et_mpeg2_decode(&p,0)==0);
    unsigned len=s[0].bitstream_len;
    for (unsigned n=0;n<len;n++) {
        s[0].bitstream_len=n;
        assert(et_mpeg2_decode(&p,0)!=0);
        assert(status[0].code!=0);
    }
    s[0].bitstream_len=gray_slice(input+2048,128,0,1); /* partial row */
    assert(et_mpeg2_decode(&p,0)==ET_DECODE_BAD_SLICE);
    s[0].bitstream_len=gray_slice(input+2048,128,0,3); /* row overrun */
    assert(et_mpeg2_decode(&p,0)==ET_DECODE_ROW_OVERFLOW);
    s[0].bitstream_len=gray_slice(input+2048,128,0,2);
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
    uint8_t temp[128]; gray_slice(temp,sizeof(temp),0,2);
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
    test_long_guarded_slices();
    puts("ET MPEG2: native gray I, 1/64 harts, row ownership, malformed/truncation tests passed");
    return 0;
}
