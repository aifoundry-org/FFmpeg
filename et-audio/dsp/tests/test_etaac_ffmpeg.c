/* Native exact test against the independently built pinned FFmpeg libraries. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "synth.h"
#include "libavcodec/aactab.h"
#include "libavcodec/sinewin.h"
#include "libavutil/cpu.h"
#include "libavutil/tx.h"

static uint32_t bits(float x)
{
    union { float f; uint32_t u; } v;
    v.f = x;
    return v.u;
}

static float from_bits(uint32_t u)
{
    union { float f; uint32_t u; } v;
    v.u = u;
    return v.f;
}

static uint32_t rng_state = UINT32_C(0x4d595df4);
static float next_float(void)
{
    union { uint32_t u; float f; } v;
    rng_state = rng_state * UINT32_C(1664525) + UINT32_C(1013904223);
    v.u = UINT32_C(0x3f000000) | (rng_state >> 9); /* [0.5, 1) */
    return (v.f - 0.75f) * 4096.0f;
}

static void mul_window(float *dst, const float *src0, const float *src1,
                       const float *win, int len)
{
    int i, j;
    dst += len;
    win += len;
    src0 += len;
    for (i = -len, j = len - 1; i < 0; i++, j--) {
        float s0 = src0[i];
        float s1 = src1[j];
        float wi = win[i];
        float wj = win[j];
        dst[i] = s0 * wj - s1 * wi;
        dst[j] = s0 * wi + s1 * wj;
    }
}

static void reference_synth(float *out, float *saved, const float *coeff,
                            unsigned sequence, unsigned previous_sequence,
                            unsigned shape, unsigned previous_shape,
                            AVTXContext *tx1024, av_tx_fn fn1024,
                            AVTXContext *tx128, av_tx_fn fn128)
{
    float buf[1024], temp[128];
    const float *swindow = shape ? ff_aac_kbd_short_128 : ff_sine_128;
    const float *lwindow_prev = previous_shape ? ff_aac_kbd_long_1024 : ff_sine_1024;
    const float *swindow_prev = previous_shape ? ff_aac_kbd_short_128 : ff_sine_128;

    if (sequence == ETAAC_EIGHT_SHORT) {
        for (int i = 0; i < 1024; i += 128)
            fn128(tx128, buf + i, (void *)(coeff + i), sizeof(float));
    } else {
        fn1024(tx1024, buf, (void *)coeff, sizeof(float));
    }

    if ((previous_sequence == ETAAC_ONLY_LONG || previous_sequence == ETAAC_LONG_STOP) &&
        (sequence == ETAAC_ONLY_LONG || sequence == ETAAC_LONG_START)) {
        mul_window(out, saved, buf, lwindow_prev, 512);
    } else {
        memcpy(out, saved, 448 * sizeof(*out));
        if (sequence == ETAAC_EIGHT_SHORT) {
            mul_window(out + 448 + 0 * 128, saved + 448,      buf + 0 * 128, swindow_prev, 64);
            mul_window(out + 448 + 1 * 128, buf + 0 * 128 + 64, buf + 1 * 128, swindow, 64);
            mul_window(out + 448 + 2 * 128, buf + 1 * 128 + 64, buf + 2 * 128, swindow, 64);
            mul_window(out + 448 + 3 * 128, buf + 2 * 128 + 64, buf + 3 * 128, swindow, 64);
            mul_window(temp,                  buf + 3 * 128 + 64, buf + 4 * 128, swindow, 64);
            memcpy(out + 448 + 4 * 128, temp, 64 * sizeof(*out));
        } else {
            mul_window(out + 448, saved + 448, buf, swindow_prev, 64);
            memcpy(out + 576, buf + 64, 448 * sizeof(*out));
        }
    }

    if (sequence == ETAAC_EIGHT_SHORT) {
        memcpy(saved, temp + 64, 64 * sizeof(*saved));
        mul_window(saved + 64,  buf + 4 * 128 + 64, buf + 5 * 128, swindow, 64);
        mul_window(saved + 192, buf + 5 * 128 + 64, buf + 6 * 128, swindow, 64);
        mul_window(saved + 320, buf + 6 * 128 + 64, buf + 7 * 128, swindow, 64);
        memcpy(saved + 448, buf + 7 * 128 + 64, 64 * sizeof(*saved));
    } else if (sequence == ETAAC_LONG_START) {
        memcpy(saved, buf + 512, 448 * sizeof(*saved));
        memcpy(saved + 448, buf + 7 * 128 + 64, 64 * sizeof(*saved));
    } else {
        memcpy(saved, buf + 512, 512 * sizeof(*saved));
    }
}

static int compare(const char *name, const float *got, const float *want, int n)
{
    for (int i = 0; i < n; i++) {
        if (bits(got[i]) != bits(want[i])) {
            fprintf(stderr, "%s[%d]: got=%08" PRIx32 " want=%08" PRIx32 "\n",
                    name, i, bits(got[i]), bits(want[i]));
            return -1;
        }
    }
    return 0;
}

int main(void)
{
    AVTXContext *tx1024 = NULL, *tx128 = NULL;
    av_tx_fn fn1024 = NULL, fn128 = NULL;
    float scale1024 = (float)((1.0 / 1024) / 32768.0f);
    float scale128 = (float)((1.0 / 128) / 32768.0f);
    float *scratch;
    int ret;

    /* The test must exercise the scalar C codelet, never a host SIMD codelet. */
    av_force_cpu_flags(0);
    ff_aac_float_common_init();
    if ((ret = av_tx_init(&tx1024, &fn1024, AV_TX_FLOAT_MDCT, 1, 1024,
                          &scale1024, 0)) < 0 ||
        (ret = av_tx_init(&tx128, &fn128, AV_TX_FLOAT_MDCT, 1, 128,
                          &scale128, 0)) < 0) {
        fprintf(stderr, "av_tx_init failed: %d\n", ret);
        return 2;
    }

    scratch = aligned_alloc(64, 4608 * sizeof(*scratch));
    if (!scratch)
        return 2;
    /* Worker protocol: the synthesis reservation ends at 4096 floats. */
    for (int i = 0; i < 512; i++)
        scratch[4096 + i] = from_bits(UINT32_C(0x3f800000) + (uint32_t)i);

    for (int nidx = 0; nidx < 2; nidx++) {
        const unsigned n = nidx ? 1024 : 128;
        float coeff[1024], got[1024], want[1024];
        AVTXContext *tx = nidx ? tx1024 : tx128;
        av_tx_fn fn = nidx ? fn1024 : fn128;
        for (int t = 0; t < 32; t++) {
            for (unsigned i = 0; i < n; i++) coeff[i] = next_float();
            fn(tx, want, coeff, sizeof(float));
            if (etaac_imdct(got, coeff, n, scratch) || compare("imdct", got, want, (int)n))
                return 1;
        }
    }

    for (unsigned previous = 0; previous < 4; previous++) {
        for (unsigned sequence = 0; sequence < 4; sequence++) {
            for (unsigned pshape = 0; pshape < 2; pshape++) {
                for (unsigned shape = 0; shape < 2; shape++) {
                    float coeff[1024], got[1024], want[1024], got_saved[512], want_saved[512];
                    for (int i = 0; i < 1024; i++) coeff[i] = next_float();
                    for (int i = 0; i < 512; i++) got_saved[i] = want_saved[i] = next_float();
                    reference_synth(want, want_saved, coeff, sequence, previous, shape, pshape,
                                    tx1024, fn1024, tx128, fn128);
                    if (etaac_synth(got, got_saved, coeff, sequence, previous, shape, pshape, scratch) ||
                        compare("synth output", got, want, 1024) ||
                        compare("synth saved", got_saved, want_saved, 512))
                        return 1;
                }
            }
        }
    }

    for (int i = 0; i < 512; i++) {
        const uint32_t want = UINT32_C(0x3f800000) + (uint32_t)i;
        if (bits(scratch[4096 + i]) != want) {
            fprintf(stderr, "transactional scratch modified at %d\n", i);
            return 1;
        }
    }

    free(scratch);
    av_tx_uninit(&tx1024);
    av_tx_uninit(&tx128);
    puts("etaac: exact scalar FFmpeg IMDCT/window tests passed");
    return 0;
}
