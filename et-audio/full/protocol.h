/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef ETAAC_FULL_PROTOCOL_H
#define ETAAC_FULL_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>
#define ETAAC_FULL_ABI 4u
#define ETAAC_FULL_INIT 0u
#define ETAAC_FULL_DECODE 1u
#define ETAAC_FULL_CLOSE 2u
#define ETAAC_FULL_METER 3u
#define ETAAC_FULL_MAX_PACKETS 2048u
#define ETAAC_FULL_STATE_BYTES 256u
#define ETAAC_FULL_STACK_BYTES (256u*1024u)
#define ETAAC_FULL_HEAP_BYTES (64u*1024u*1024u)
#define ETAAC_FULL_MAGIC UINT64_C(0x34434141465445)
/* One independent stream, hart 0 only. All allocations are owned/disjoint.
 * input = 64-byte header + packet_count 64-byte descriptors + padded ADTS packets.
 * output = packet-major planar [packet][channel][1024] F32, no priming trim.
 * scratch = bounded arena plus final STACK_BYTES reserved for private stack. */
typedef struct ETAACFullParams {
 uint32_t abi_version,operation,count,flags;
 uint64_t input_addr,input_bytes,state_addr,state_bytes,output_addr,output_bytes;
 uint64_t scratch_addr,scratch_bytes,status_addr,status_bytes;
 uint64_t generation;
 uint32_t first_packet,packets,capacity_frames,reserved;
 uint64_t reserved2;
} ETAACFullParams;
typedef struct ETAACFullInput {
 uint64_t magic,bytes;
 uint32_t packet_count,channels,sample_rate,reserved0;
 uint64_t reserved[4];
} ETAACFullInput;
typedef struct ETAACFullPacket {
 uint64_t offset;
 uint32_t bytes,reserved0;
 uint64_t reserved[6];
} ETAACFullPacket;
typedef struct ETAACFullStatus {
 uint64_t generation;
 int32_t result;
 uint32_t operation;
 uint64_t frames,samples,heap_peak,init_ticks,decode_ticks,pcm_publish_ticks;
 uint64_t consumer_peak_bits,consumer_above_one,unsupported_calls,heap_failures;
 uint64_t context_publish_ticks,heap_live,stack_guard_ok,reserved;
} ETAACFullStatus;
#if defined(__cplusplus)
static_assert(sizeof(ETAACFullParams)==128 && sizeof(ETAACFullInput)==64 && sizeof(ETAACFullPacket)==64 && sizeof(ETAACFullStatus)==128,"full AAC ABI");
#else
_Static_assert(sizeof(ETAACFullParams)==128 && sizeof(ETAACFullInput)==64 && sizeof(ETAACFullPacket)==64 && sizeof(ETAACFullStatus)==128,"full AAC ABI");
#endif
static inline int etaac_full_valid(const ETAACFullParams *p) {
 uint64_t a[5],n[5];
 if(!p||p->abi_version!=ETAAC_FULL_ABI||p->operation>ETAAC_FULL_METER||p->count!=1||p->flags||p->reserved||p->reserved2||!p->generation||!p->capacity_frames||p->capacity_frames>ETAAC_FULL_MAX_PACKETS||p->packets>p->capacity_frames||p->first_packet>p->capacity_frames-p->packets)return 0;
 if((p->operation==ETAAC_FULL_DECODE||p->operation==ETAAC_FULL_METER)&&(!p->packets||p->generation!=p->first_packet+1))return 0;
 if(p->input_bytes<128||p->input_bytes>(64u*1024u*1024u)||p->state_bytes!=ETAAC_FULL_STATE_BYTES||p->scratch_bytes!=ETAAC_FULL_HEAP_BYTES+ETAAC_FULL_STACK_BYTES||p->status_bytes!=sizeof(ETAACFullStatus)||
    (p->output_bytes!=(uint64_t)p->capacity_frames*1024*4&&p->output_bytes!=(uint64_t)p->capacity_frames*2*1024*4))return 0;
 a[0]=p->input_addr;n[0]=p->input_bytes;a[1]=p->state_addr;n[1]=p->state_bytes;a[2]=p->output_addr;n[2]=p->output_bytes;a[3]=p->scratch_addr;n[3]=p->scratch_bytes;a[4]=p->status_addr;n[4]=p->status_bytes;
 for(unsigned i=0;i<5;i++){if(!a[i]||(a[i]&63)||(n[i]&63)||a[i]>UINT64_MAX-n[i])return 0;for(unsigned j=0;j<i;j++)if(a[i]<a[j]+n[j]&&a[j]<a[i]+n[i])return 0;}return 1;
}
/* Shared packet-envelope validation. Codec syntax is still decoded on ET. */
static inline int etaac_full_input_valid(const void *data,uint64_t bytes,uint32_t capacity) {
 const ETAACFullInput *h=(const ETAACFullInput *)data;
 if(!h||bytes<128||bytes>(64u*1024u*1024u)||(bytes&63)||h->magic!=ETAAC_FULL_MAGIC||h->bytes!=bytes||!h->packet_count||h->packet_count>ETAAC_FULL_MAX_PACKETS||h->packet_count!=capacity||(h->channels!=1&&h->channels!=2)||(h->sample_rate!=44100&&h->sample_rate!=48000)||h->reserved0)return 0;
 for(unsigned i=0;i<4;i++)if(h->reserved[i])return 0;
 uint64_t cursor=64+(uint64_t)h->packet_count*64;
 if(cursor>bytes)return 0;
 const ETAACFullPacket *q=(const ETAACFullPacket *)((const unsigned char *)data+64);
 for(unsigned i=0;i<h->packet_count;i++) {
  if(q[i].offset!=cursor||q[i].bytes<8||q[i].bytes>8191||q[i].reserved0||cursor>bytes||q[i].bytes+64u>bytes-cursor)return 0;
  for(unsigned j=0;j<6;j++)if(q[i].reserved[j])return 0;
  const unsigned char *b=(const unsigned char *)data+cursor;
  unsigned rate=(b[2]>>2)&15,channels=((b[2]&1)<<2)|(b[3]>>6);
  unsigned length=((b[3]&3)<<11)|(b[4]<<3)|(b[5]>>5);
  if(b[0]!=255||(b[1]!=0xf1&&b[1]!=0xf9)||(b[2]>>6)!=1||(rate!=3&&rate!=4)||channels!=h->channels||(rate==3?48000u:44100u)!=h->sample_rate||length!=q[i].bytes||(b[6]&3))return 0;
  uint64_t next=(cursor+q[i].bytes+64+63)&~UINT64_C(63);
  if(next>bytes)return 0;
  for(uint64_t j=cursor+q[i].bytes;j<next;j++)if(((const unsigned char *)data)[j])return 0;
  cursor=next;
 }
 return cursor==bytes;
}
int etaac_full_process(const ETAACFullParams *p);
#endif
