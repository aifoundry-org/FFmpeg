/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "protocol.h"
#include "arena.h"
#include "platform.h"
#include "../../et-kernels/src/cache.h"
#include <errno.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavcodec/codec_internal.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>

/* Result codes are private, negative and fail closed. */
enum { FULL_OK=0, FULL_BAD_PARAMS=-1, FULL_BAD_INPUT=-2, FULL_BAD_STATE=-3,
       FULL_UNSUPPORTED=-4, FULL_NOMEM=-5, FULL_DECODER=-6, FULL_ABORT=-7,
       FULL_PLATFORM=-8 };
#define STATE_MAGIC UINT64_C(0x3445544153544154)
typedef struct PrivateState {
    uint64_t magic, scratch_addr, heap_bytes;
    uint64_t ctx, frame, input_addr, input_bytes, next_offset;
    uint32_t initialized, next_packet, channels, sample_rate;
    uint32_t sample_index, reserved0;
    uint64_t resident_peak, decoder_count, faults;
    uint8_t reserved[144];
} PrivateState;
_Static_assert(sizeof(PrivateState)==ETAAC_FULL_STATE_BYTES, "private state ABI");
typedef struct Globals {
    ETAACFullArena arena;
    AVCodecContext *ctx;
    AVFrame *frame;
    uint64_t state_addr, scratch_addr;
    uint32_t live;
    jmp_buf escape;
} Globals;
/* Explicit initializer forces this object into image data; linker makes all
 * FFmpeg BSS likewise PROGBITS zeroes for the legacy loader. */
static Globals globals = {0};
extern const FFCodec ff_aac_decoder;

typedef uint32_t etaac_alias_u32 __attribute__((may_alias));
typedef struct ADTS {
    const uint8_t *payload;
    uint32_t payload_bytes;
    uint32_t rate, channels, index;
} ADTS;
static uint32_t bebits(const uint8_t *p, unsigned bit, unsigned n) {
    uint32_t v=0; while(n--) { v=(v<<1)|((p[bit>>3]>>(7-(bit&7)))&1u); bit++; } return v;
}
static int parse_adts(const uint8_t *p, uint32_t n, ADTS *a) {
    static const uint32_t rates[16]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350,0,0,0};
    uint32_t profile, index, channels, length;
    if(n<7 || p[0]!=0xff || (p[1]&0xf6)!=0xf0 || !(p[1]&1) || (p[6]&3)) return FULL_BAD_INPUT;
    profile=bebits(p,16,2); index=bebits(p,18,4); channels=bebits(p,23,3); length=bebits(p,30,13);
    if(profile!=1 || index>15 || (rates[index]!=44100 && rates[index]!=48000) || (channels!=1 && channels!=2) || length!=n) return FULL_UNSUPPORTED;
    a->payload=p+7; a->payload_bytes=n-7; a->rate=rates[index]; a->channels=channels; a->index=index;
    return a->payload_bytes ? FULL_OK : FULL_BAD_INPUT;
}
/* The shared ABI validator owns descriptor/padding/gap validation.  After a
 * successful INIT the complete immutable input blob is pinned in state; later
 * launches only parse the requested already-validated descriptor. */
static int packet_at(const ETAACFullParams *p, const ETAACFullInput *h, uint32_t i,
                     uint64_t expected_offset, ADTS *a, uint64_t *next_offset) {
    const ETAACFullPacket *d; uint64_t end,next;
    if(i>=h->packet_count)return FULL_BAD_INPUT;
    d=(const ETAACFullPacket *)((const uint8_t *)h+64u+(uint64_t)i*64u);
    /* INIT checked the whole envelope once.  Every consumed descriptor is
     * rechecked locally so post-DMA mutation cannot redirect a decoder read. */
    if(d->offset!=expected_offset || (d->offset&63) || d->bytes<8 || d->bytes>8191 || d->reserved0)return FULL_BAD_INPUT;
    for(unsigned k=0;k<6;k++)if(d->reserved[k])return FULL_BAD_INPUT;
    if(d->offset>p->input_bytes-d->bytes)return FULL_BAD_INPUT;
    end=d->offset+d->bytes;
    if(end>UINT64_MAX-64)return FULL_BAD_INPUT;
    next=(end+64+63)&~UINT64_C(63);
    if(next<end || next>p->input_bytes)return FULL_BAD_INPUT;
    for(uint64_t k=end;k<next;k++)if(((const uint8_t *)h)[k])return FULL_BAD_INPUT;
    if(parse_adts((const uint8_t *)h+d->offset,d->bytes,a))return FULL_BAD_INPUT;
    *next_offset=next; return FULL_OK;
}
static int input_identity(const ETAACFullParams *p, const PrivateState *s, ETAACFullInput **out) {
    ETAACFullInput *h=(ETAACFullInput *)(uintptr_t)p->input_addr;
    if(p->input_addr!=s->input_addr || p->input_bytes!=s->input_bytes ||
       h->magic!=ETAAC_FULL_MAGIC || h->bytes!=p->input_bytes || h->packet_count!=p->capacity_frames ||
       h->channels!=s->channels || h->sample_rate!=s->sample_rate)return FULL_BAD_INPUT;
    *out=h; return FULL_OK;
}
static int output_ok(const ETAACFullParams *p, uint32_t channels) {
    uint64_t want=(uint64_t)p->capacity_frames*channels*1024u*sizeof(float);
    return p->output_bytes==want ? FULL_OK : FULL_BAD_PARAMS;
}
#ifdef ET_DEVICE
extern unsigned char __aac_globals_start[],__aac_globals_end[];
#endif
#define STACK_SENTINEL UINT64_C(0xd7d7d7d7d7d7d7d7)
static int stack_ok(const ETAACFullParams *p) {
#ifdef ET_DEVICE
 const volatile uint64_t *g=(const volatile uint64_t *)(uintptr_t)(p->scratch_addr+ETAAC_FULL_HEAP_BYTES);
 for(unsigned i=0;i<8;i++)if(g[i]!=STACK_SENTINEL)return 0;
#else
 (void)p;
#endif
 return 1;
}
static void publish(const ETAACFullParams *p, int rc, uint64_t frames, uint64_t samples, uint64_t ti, uint64_t td, uint64_t tp, const PrivateState *s) {
    ETAACFullStatus *st=(ETAACFullStatus *)(uintptr_t)p->status_addr;
    memset(st,0,sizeof(*st)); st->generation=p->generation; st->result=rc; st->operation=p->operation;
    st->frames=frames; st->samples=samples; st->heap_peak=globals.arena.peak; st->init_ticks=ti; st->decode_ticks=td; st->pcm_publish_ticks=tp;
    st->unsupported_calls=etaac_platform_faults(); st->heap_failures=globals.arena.failures;
    if(s) { if(!st->unsupported_calls)st->unsupported_calls=s->faults; st->consumer_peak_bits=s->resident_peak; st->consumer_above_one=s->decoder_count; }
    st->stack_guard_ok=stack_ok(p);st->heap_live=globals.arena.used;
    if(!st->stack_guard_ok)st->result=FULL_BAD_STATE;
    /* Conservatively publish mutable library tables/globals and the entire
     * live heap prefix between launches, not just the pointer-bearing state.
     * Do not rely on an undocumented cache lifetime across kernel launches. */
    uint64_t begin=et_cycles();
    uint64_t span=etaac_arena_publish_bytes(&globals.arena);
    if(span)et_evict(globals.arena.base,(size_t)span);
#ifdef ET_DEVICE
    et_evict(__aac_globals_start,(uintptr_t)__aac_globals_end-(uintptr_t)__aac_globals_start);
#endif
    st->context_publish_ticks=et_cycles()-begin;
    st->heap_failures=globals.arena.failures;
    if(st->heap_failures&&!st->result)st->result=FULL_NOMEM;
    et_evict(st,sizeof(*st));
}
/* Called by entry.S while still on firmware stack.  It deliberately reads no
 * globals and does no status write for an untrustworthy pointer/range. */
typedef struct ETKernelEnvironment { uint16_t major,minor,patch,reserved; uint64_t shire_mask; uint32_t frequency,padding; } ETKernelEnvironment;
uintptr_t etaac_full_boot_stack(const ETAACFullParams *p, const ETKernelEnvironment *env, uintptr_t firmware_sp) {
    if(!etaac_full_valid(p) || !env || env->shire_mask!=1 || !firmware_sp) return 0;
    etaac_platform_set_firmware_sp(firmware_sp);
    volatile uint64_t *g=(volatile uint64_t *)(uintptr_t)(p->scratch_addr+ETAAC_FULL_HEAP_BYTES);
    for(unsigned i=0;i<8;i++)g[i]=STACK_SENTINEL;
    return (uintptr_t)p->scratch_addr+p->scratch_bytes;
}
static void no_log(void *avcl, int level, const char *fmt, va_list vl) { (void)avcl;(void)level;(void)fmt;(void)vl; }
static int state_bound(const ETAACFullParams *p, const PrivateState *s) {
    if(s->magic!=STATE_MAGIC || s->initialized!=1 || s->reserved0 || s->next_packet>p->capacity_frames ||
       s->next_offset> s->input_bytes || s->scratch_addr!=p->scratch_addr || s->heap_bytes!=ETAAC_FULL_HEAP_BYTES ||
       globals.live!=1 || globals.state_addr!=p->state_addr || globals.scratch_addr!=p->scratch_addr ||
       !globals.ctx || !globals.frame || s->ctx!=(uintptr_t)globals.ctx || s->frame!=(uintptr_t)globals.frame ||
       !etaac_arena_owns(&globals.arena,globals.ctx) || !etaac_arena_owns(&globals.arena,globals.frame)) return 0;
    for(unsigned i=0;i<sizeof(s->reserved);i++) if(s->reserved[i])return 0;
    return 1;
}
static void bind_new_state(PrivateState *s) {
    globals.state_addr=(uintptr_t)s; globals.live=1;
}
static int create_decoder(PrivateState *s, const ADTS *a, const ETAACFullParams *p) {
    const AVCodec *codec; uint8_t *asc;
    etaac_arena_init(&globals.arena,(void *)(uintptr_t)p->scratch_addr,ETAAC_FULL_HEAP_BYTES);
    etaac_platform_set_arena(&globals.arena); etaac_platform_reset_faults(); av_log_set_callback(no_log);
    codec=&ff_aac_decoder.p;
    globals.ctx=avcodec_alloc_context3(codec); globals.frame=av_frame_alloc();
    if(!globals.ctx || !globals.frame)return FULL_NOMEM;
    asc=av_mallocz(2+AV_INPUT_BUFFER_PADDING_SIZE); if(!asc)return FULL_NOMEM;
    asc[0]=(uint8_t)((2u<<3)|(a->index>>1)); asc[1]=(uint8_t)((a->index&1u)<<7|(a->channels<<3));
    globals.ctx->extradata=asc; globals.ctx->extradata_size=2; globals.ctx->request_sample_fmt=AV_SAMPLE_FMT_FLTP;
    if(avcodec_open2(globals.ctx,codec,0)<0)return FULL_DECODER;
    memset(s,0,sizeof(*s)); s->magic=STATE_MAGIC; s->scratch_addr=p->scratch_addr; s->heap_bytes=ETAAC_FULL_HEAP_BYTES;
    s->ctx=(uintptr_t)globals.ctx; s->frame=(uintptr_t)globals.frame; s->input_addr=p->input_addr; s->input_bytes=p->input_bytes;
    s->next_offset=64u+(uint64_t)((ETAACFullInput *)(uintptr_t)p->input_addr)->packet_count*64u;
    s->initialized=1; s->channels=a->channels; s->sample_rate=a->rate; s->sample_index=a->index;
    s->resident_peak=0; s->decoder_count=0; globals.scratch_addr=p->scratch_addr; bind_new_state(s); return FULL_OK;
}
static int pcm_measure(const float *x, uint32_t n, uint64_t *peak, uint64_t *above) {
    /* AV_SAMPLE_FMT_FLTP planes are at least float-aligned; may_alias keeps
     * this exact IEEE-bit validation from changing scalar FP arithmetic. */
    const etaac_alias_u32 *bits=(const etaac_alias_u32 *)x;
    for(uint32_t i=0;i<n;i++) { uint32_t u=bits[i]&UINT32_C(0x7fffffff);
        if(u>=UINT32_C(0x7f800000))return FULL_DECODER;
        if(u>*peak)*peak=u;
        if(u>UINT32_C(0x3f800000))(*above)++;
    } return FULL_OK;
}
static int decode_one(const ETAACFullParams *p, PrivateState *s, const ADTS *a, uint32_t index, uint64_t *pcm_publish_ticks) {
    AVPacket *pkt; int rc, more; float *dst; uint64_t peak=s->resident_peak, above=s->decoder_count;
    if(a->channels!=s->channels || a->rate!=s->sample_rate || a->index!=s->sample_index)return FULL_UNSUPPORTED;
    pkt=av_packet_alloc(); if(!pkt)return FULL_NOMEM;
    rc=av_new_packet(pkt,(int)a->payload_bytes); if(rc<0){av_packet_free(&pkt);return FULL_NOMEM;}
    memcpy(pkt->data,a->payload,a->payload_bytes);
    rc=avcodec_send_packet(globals.ctx,pkt); av_packet_free(&pkt); if(rc<0)return FULL_DECODER;
    rc=avcodec_receive_frame(globals.ctx,globals.frame); if(rc<0)return FULL_DECODER;
    if(globals.ctx->profile!=AV_PROFILE_AAC_LOW || globals.frame->format!=AV_SAMPLE_FMT_FLTP || globals.frame->nb_samples!=1024 ||
       globals.frame->sample_rate!=(int)s->sample_rate || globals.frame->ch_layout.nb_channels!=(int)s->channels ||
       !globals.frame->extended_data)return FULL_UNSUPPORTED;
    dst=(float *)(uintptr_t)p->output_addr+(uint64_t)index*s->channels*1024u;
    for(uint32_t c=0;c<s->channels;c++) { const float *plane;
        if(!globals.frame->extended_data[c])return FULL_DECODER;
        plane=(const float *)globals.frame->extended_data[c]; rc=pcm_measure(plane,1024,&peak,&above); if(rc)return rc;
        memcpy(dst+(uint64_t)c*1024u,plane,1024u*sizeof(float));
    }
    s->resident_peak=peak; s->decoder_count=above;
    av_frame_unref(globals.frame);
    more=avcodec_receive_frame(globals.ctx,globals.frame); if(more!=AVERROR(EAGAIN)) { if(more>=0)av_frame_unref(globals.frame); return FULL_UNSUPPORTED; }
    { uint64_t t=et_cycles(); et_evict(dst,(size_t)s->channels*1024u*sizeof(float)); *pcm_publish_ticks+=et_cycles()-t; }
    return FULL_OK;
}
static void poison(PrivateState *s) { s->initialized=2; et_evict(s,sizeof(*s)); }
int etaac_full_process(const ETAACFullParams *p) {
    ETAACFullInput *h=0; PrivateState *s; ADTS a; int rc=FULL_OK;
    uint64_t ti=0,td=0,tp=0,t,frames=0,next_offset=0;
    if(!etaac_full_valid(p))return FULL_BAD_PARAMS;
    s=(PrivateState *)(uintptr_t)p->state_addr;
    /* All reachable FFmpeg/newlib calls run below this escape. abort() never
     * returns; it transfers here and publishes a fail-closed result. */
    if(setjmp(globals.escape)) {
        etaac_platform_clear_escape();
        if(p->operation!=ETAAC_FULL_INIT && s->magic==STATE_MAGIC) poison(s);
        publish(p,FULL_ABORT,0,0,0,0,0,p->operation==ETAAC_FULL_INIT?0:s);
        return FULL_ABORT;
    }
    etaac_platform_set_escape(&globals.escape);
    if(p->operation==ETAAC_FULL_INIT) {
        if(p->generation!=1 || p->first_packet || p->packets || s->magic==STATE_MAGIC ||
           !etaac_full_input_valid((const void *)(uintptr_t)p->input_addr,p->input_bytes,p->capacity_frames)) rc=FULL_BAD_INPUT;
        h=(ETAACFullInput *)(uintptr_t)p->input_addr;
        if(!rc) rc=packet_at(p,h,0,64u+(uint64_t)h->packet_count*64u,&a,&next_offset);
        if(!rc && (a.channels!=h->channels || a.rate!=h->sample_rate || (rc=output_ok(p,a.channels)))) { if(!rc)rc=FULL_BAD_INPUT; }
        if(!rc) { t=et_cycles(); rc=create_decoder(s,&a,p); ti=et_cycles()-t; }
        if(!rc && etaac_platform_faults()) { poison(s); rc=FULL_PLATFORM; }
        if(!rc) et_evict(s,sizeof(*s));
        etaac_platform_clear_escape(); publish(p,rc,0,0,ti,0,0,rc?0:s); return rc;
    }
    if(!state_bound(p,s)) { etaac_platform_clear_escape(); publish(p,FULL_BAD_STATE,0,0,0,0,0,0); return FULL_BAD_STATE; }
    etaac_platform_set_arena(&globals.arena);
    if(p->operation==ETAAC_FULL_CLOSE) {
        if(p->first_packet || p->packets || p->generation!=(uint64_t)p->capacity_frames+1) { etaac_platform_clear_escape(); publish(p,FULL_BAD_PARAMS,0,0,0,0,0,s); return FULL_BAD_PARAMS; }
        etaac_platform_reset_faults(); t=et_cycles();
        if(s->next_packet==p->capacity_frames && (avcodec_send_packet(globals.ctx,NULL)<0 || avcodec_receive_frame(globals.ctx,globals.frame)!=AVERROR_EOF)) {
            poison(s);etaac_platform_clear_escape();publish(p,FULL_DECODER,0,0,0,0,0,s);return FULL_DECODER;
        }
        avcodec_free_context(&globals.ctx); av_frame_free(&globals.frame); ti=et_cycles()-t;
        rc=etaac_platform_faults()?FULL_PLATFORM:FULL_OK;
        memset(s,0,sizeof(*s)); globals.live=0; globals.ctx=0; globals.frame=0; etaac_platform_set_arena(0); et_evict(s,sizeof(*s));
        etaac_platform_clear_escape(); publish(p,rc,0,0,ti,0,0,0); return rc;
    }
    if(p->operation==ETAAC_FULL_METER) {
        rc=(p->first_packet || p->packets!=p->capacity_frames || p->generation!=1 || s->next_packet!=p->capacity_frames) ? FULL_BAD_PARAMS : FULL_OK;
        etaac_platform_clear_escape(); publish(p,rc,0,0,0,0,0,s); return rc;
    }
    if((rc=output_ok(p,s->channels)) || p->first_packet!=s->next_packet || (rc=input_identity(p,s,&h))) { etaac_platform_clear_escape(); publish(p,rc?rc:FULL_BAD_STATE,0,0,0,0,0,s); return rc?rc:FULL_BAD_STATE; }
    etaac_platform_reset_faults(); t=et_cycles();
    next_offset=s->next_offset;
    for(uint32_t i=0; i<p->packets; i++) {
        rc=packet_at(p,h,p->first_packet+i,next_offset,&a,&next_offset);
        if(!rc)rc=decode_one(p,s,&a,p->first_packet+i,&tp);
        if(rc)break;
        frames++;
    }
    td=et_cycles()-t;
    if(!rc && etaac_platform_faults())rc=FULL_PLATFORM;
    if(!rc) { s->next_packet+=p->packets; s->next_offset=next_offset; s->faults+=etaac_platform_faults(); et_evict(s,sizeof(*s)); }
    else poison(s);
    etaac_platform_clear_escape(); publish(p,rc,frames,frames*1024u,0,td,tp,s); return rc;
}
