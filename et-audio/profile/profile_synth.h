/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef ETAAC_PROFILE_SYNTH_H
#define ETAAC_PROFILE_SYNTH_H
#include "../dsp/synth.h"
#include "../../et-kernels/src/cache.h"
typedef struct ETAACProfileMeasure {
    uint64_t imdct_fft_ticks;
    uint64_t window_overlap_ticks;
    unsigned enabled;
} ETAACProfileMeasure;
int etaac_profile_synth(float *out1024, float *saved512, const float *coeff1024,
                        unsigned sequence, unsigned previous_sequence,
                        unsigned shape, unsigned previous_shape, float *scratch,
                        ETAACProfileMeasure *measure);
#endif
