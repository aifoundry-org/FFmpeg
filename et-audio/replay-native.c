/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "capture_io.h"
#include "dsp/synth.h"
int main(int argc,char **argv) {
    if(argc!=2) return 2;
    ETAACCapture c={0};if(capture_load(&c,argv[1]))return 1;
    float out[1024],saved[512];void *scratch=NULL;
    if(posix_memalign(&scratch,64,4096*sizeof(float)))return 1;
    unsigned frames=0;
    for(unsigned i=0;i<c.contexts;i++) for(unsigned j=0;j<c.counts[i];j++) {
        const ETAACRecord *r=capture_record(&c,i,j);
        if(!j)memcpy(saved,r->before,sizeof(saved));
        if(memcmp(saved,r->before,sizeof(saved)) ||
           etaac_synth(out,saved,r->coeff,r->sequence,r->previous_sequence,r->shape,r->previous_shape,scratch) ||
           memcmp(out,r->output,sizeof(out)) || memcmp(saved,r->after,sizeof(saved))) {
            fprintf(stderr,"capture mismatch context%u frame%u\n",i+1,j);return 1;
        }
        frames++;
    }
    printf("PASS capture replay: %u exact channel frames, continuous resident overlap (%u contexts)\n",frames,c.contexts);
    free(scratch);capture_free(&c);return 0;
}
