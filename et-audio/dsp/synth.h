/*
 * Freestanding AAC-LC float IMDCT/window prototype.
 *
 * Derived from FFmpeg n7.1.1 scalar transform and AAC decoder code.
 * See PROVENANCE.md.  SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef ET_AUDIO_DSP_SYNTH_H
#define ET_AUDIO_DSP_SYNTH_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ONLY_LONG   = 0,
    LONG_START  = 1,
    EIGHT_SHORT = 2,
    LONG_STOP   = 3,
};

enum {
    ETAAC_SINE = 0,
    ETAAC_KBD  = 1,
};

/* Prefixed aliases are provided for callers that avoid generic enum names. */
enum {
    ETAAC_ONLY_LONG   = ONLY_LONG,
    ETAAC_LONG_START  = LONG_START,
    ETAAC_EIGHT_SHORT = EIGHT_SHORT,
    ETAAC_LONG_STOP   = LONG_STOP,
};

/*
 * Produce the AAC decoder's half-IMDCT layout for n spectral coefficients.
 * n must be 128 or 1024. out, coeff, and scratch must not overlap. scratch
 * has at least n floats; it is 64-byte aligned in the synthesis API.
 */
int etaac_imdct(float *out, const float *coeff, unsigned n, float *scratch);

/*
 * Scalar AAC-LC inverse transform, windowing, and saved-overlap update.
 * The output is one 1024-sample PCM block. saved512 carries decoder state.
 * coeff1024 holds either one long transform or eight consecutive short ones.
 * sequence/previous_sequence are ONLY_LONG..LONG_STOP; shape/previous_shape
 * are ETAAC_SINE or ETAAC_KBD. out1024, saved512, coeff1024, and scratch must
 * not overlap. The worker protocol supplies 4608 64-byte-aligned floats:
 * this routine accesses only scratch[0..4095]; scratch[4096..4607] is
 * transactional state owned by the caller and is never touched.
 * Returns 0 on success or -1 for an unsupported selector/null argument.
 */
int etaac_synth(float *out1024, float *saved512, const float *coeff1024,
                unsigned sequence, unsigned previous_sequence,
                unsigned shape, unsigned previous_shape, float *scratch);

#ifdef __cplusplus
}
#endif
#endif
