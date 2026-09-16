/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Host-only strict reader for capture/aac_capture_format.h. */
#ifndef ETAAC_CAPTURE_IO_H
#define ETAAC_CAPTURE_IO_H
#include "capture/aac_capture_format.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
typedef struct ETAACRecord {
    uint32_t context,frame,sequence,previous_sequence,shape,previous_shape;
    float coeff[1024],before[512],output[1024],after[512];
} ETAACRecord;
_Static_assert(sizeof(ETAACRecord)==ETAAC_CAPTURE_RECORD_BYTES,"capture ABI");
typedef struct ETAACCapture {
    ETAACRecord *records;
    const ETAACRecord **index;
    unsigned contexts,counts[64],offsets[64],selected[64],selected_count;
} ETAACCapture;
static void capture_free(ETAACCapture *c) {free(c->records);free(c->index);memset(c,0,sizeof(*c));}
static int capture_load(ETAACCapture *c,const char *path) {
    uint32_t hdr[6],little=1;char magic[8];int result=-1;
    FILE *f=fopen(path,"rb");if(!f)return -1;
    if(*(unsigned char*)&little!=1 || fseek(f,0,SEEK_END))goto done;
    long sz=ftell(f);if(sz<32 || sz>256*1024*1024 || (sz-32)%sizeof(ETAACRecord))goto done;
    rewind(f);
    if(fread(magic,1,8,f)!=8 || memcmp(magic,ETAAC_CAPTURE_MAGIC,8) ||
       fread(hdr,4,6,f)!=6 || hdr[0]!=1 || hdr[1]!=32 || hdr[2]!=sizeof(ETAACRecord) ||
       hdr[3]!=1 || hdr[4]!=ETAAC_CAPTURE_ENDIAN_TAG || hdr[5])goto done;
    size_t n=(sz-32)/sizeof(ETAACRecord);if(!n)goto done;
    c->records=malloc(n*sizeof(ETAACRecord));c->index=calloc(n,sizeof(*c->index));
    if(!c->records || !c->index || fread(c->records,sizeof(ETAACRecord),n,f)!=n)goto done;
    for(size_t i=0;i<n;i++) {
        ETAACRecord *r=c->records+i;
        if(!r->context || r->context>64 || r->sequence>3 || r->previous_sequence>3 ||
           r->shape>1 || r->previous_shape>1)goto done;
        unsigned slot=r->context-1;
        if(r->frame!=c->counts[slot]++)goto done;
        if(r->context>c->contexts)c->contexts=r->context;
    }
    for(unsigned i=0;i<c->contexts;i++) {
        if(!c->counts[i])goto done;
        if(i)c->offsets[i]=c->offsets[i-1]+c->counts[i-1];
    }
    for(size_t i=0;i<n;i++) {
        const ETAACRecord *r=c->records+i;
        c->index[c->offsets[r->context-1]+r->frame]=r;
    }
    /* Ensure every saved-state/window chain is continuous BEFORE any runtime
     * initialization. Corrupt/incomplete fixtures cannot silently seed frames. */
    for(unsigned i=0;i<c->contexts;i++) for(unsigned j=1;j<c->counts[i];j++) {
        const ETAACRecord *r=c->index[c->offsets[i]+j],*prev=c->index[c->offsets[i]+j-1];
        if(memcmp(r->before,prev->after,sizeof(r->before)) || r->previous_sequence!=prev->sequence || r->previous_shape!=prev->shape)goto done;
    }
    result=0;
done:
    fclose(f);if(result)capture_free(c);return result;
}
/* FFmpeg demux probing may create separate short-lived decoder contexts.
 * Select complete timelines explicitly; never splice different state IDs. */
static int capture_select(ETAACCapture *c,unsigned frames) {
    c->selected_count=0;
    for(unsigned i=0;i<c->contexts;i++) if(c->counts[i]>=frames)
        c->selected[c->selected_count++]=i;
    return c->selected_count?0:-1;
}
static const ETAACRecord *capture_record(const ETAACCapture *c,unsigned slot,unsigned frame) {
    unsigned context=c->selected_count?c->selected[slot%c->selected_count]:slot%c->contexts;
    return c->index[c->offsets[context]+frame];
}
#endif
