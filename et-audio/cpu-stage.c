/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Optimized CPU synthesis-stage benchmark, NOT a full AAC decode benchmark.
 * Uses pinned av_tx/float_dsp dispatch and extracted scalar window sequencing.
 */
#define _POSIX_C_SOURCE 200809L
#include "capture_io.h"
#include "dsp/synth.h"
#include "libavcodec/aactab.h"
#include "libavcodec/sinewin.h"
#include "libavutil/tx.h"
#include "libavutil/cpu.h"
#include "libavutil/float_dsp.h"
#include "libavutil/mem.h"
#include <time.h>
#include <math.h>
static AVFloatDSPContext *fdsp;
#include "cpu_reference_synth.h"
static double now(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(int argc,char **argv) {
    if((argc!=4 && argc!=5) || (argc==5 && strcmp(argv[4],"scalar"))) {fprintf(stderr,"usage: cpu-stage CAPTURE TASKS REPEATS [scalar]\n");return 2;}
    int scalar=argc==5;
    if(scalar)av_force_cpu_flags(0);
    unsigned tasks=(unsigned)atoi(argv[2]),repeats=(unsigned)atoi(argv[3]);
    if(!tasks||tasks>64||!repeats||repeats>10000)return 2;
    ETAACCapture capture={0};if(capture_load(&capture,argv[1]))return 1;
    unsigned frames=0;for(unsigned i=0;i<capture.contexts;i++)if(capture.counts[i]>frames)frames=capture.counts[i];
    if(capture_select(&capture,frames))return 1;
    ff_aac_float_common_init();
    AVTXContext *tx1024=NULL,*tx128=NULL;av_tx_fn fn1024,fn128;
    float scale1024=(1.0f/1024)/32768.0f,scale128=(1.0f/128)/32768.0f;
    fdsp=avpriv_float_dsp_alloc(0);if(!fdsp)return 1;
    if(av_tx_init(&tx1024,&fn1024,AV_TX_FLOAT_MDCT,1,1024,&scale1024,0)<0 ||
       av_tx_init(&tx128,&fn128,AV_TX_FLOAT_MDCT,1,128,&scale128,0)<0)return 1;
    _Alignas(64) float coeff[1024],out[1024],saved[64][512];
    unsigned mismatches=0;double max_abs=0.0;
    /* Characterize, do not silently replace, scalar golden arithmetic. */
    for(unsigned f=0;f<frames;f++) for(unsigned i=0;i<tasks;i++) {
        const ETAACRecord *r=capture_record(&capture,i,f);
        if(!f)memcpy(saved[i],r->before,sizeof(saved[i]));
        memcpy(coeff,r->coeff,sizeof(coeff));
        reference_synth(out,saved[i],coeff,r->sequence,r->previous_sequence,r->shape,r->previous_shape,tx1024,fn1024,tx128,fn128);
        for(unsigned j=0;j<1024;j++) {
            if(memcmp(out+j,r->output+j,4))mismatches++;
            double e=fabs((double)out[j]-r->output[j]);if(e>max_abs)max_abs=e;
        }
    }
    if(scalar && mismatches) {fprintf(stderr,"scalar reference mismatch\n");return 1;}
    double checksum=0.0,start=now();
    for(unsigned rep=0;rep<repeats;rep++) for(unsigned f=0;f<frames;f++) for(unsigned i=0;i<tasks;i++) {
        const ETAACRecord *r=capture_record(&capture,i,f);
        if(!f)memcpy(saved[i],r->before,sizeof(saved[i]));
        memcpy(coeff,r->coeff,sizeof(coeff));
        reference_synth(out,saved[i],coeff,r->sequence,r->previous_sequence,r->shape,r->previous_shape,tx1024,fn1024,tx128,fn128);
        checksum+=out[f%1024];
    }
    double elapsed=now()-start;
    printf("{\"type\":\"cpu_stage\",\"scalar\":%d,\"tasks\":%u,\"frames_per_session\":%u,\"repeats\":%u,\"cpu_flags\":%d,\"elapsed_s\":%.9f,\"batch_s\":%.9f,\"scalar_golden_different_samples\":%u,\"max_abs_error\":%.17g,\"checksum\":%.17g}\n",scalar,tasks,frames,repeats,av_get_cpu_flags(),elapsed,elapsed/(repeats*frames),mismatches,max_abs,checksum);
    av_tx_uninit(&tx1024);av_tx_uninit(&tx128);av_free(fdsp);capture_free(&capture);return 0;
}
