/*
 * Freestanding AAC-LC float IMDCT + overlap/window scalar prototype.
 *
 * Derived from FFmpeg n7.1.1 libavutil/tx_template.c, tx_priv.h, tx.c,
 * libavcodec/aac/aacdec_dsp_template.c, and libavutil/float_dsp.c.
 * Those sources credit Lynne, Loren Merritt, Fabrice Bellard, D. J. Bernstein,
 * Balatoni Denes, Oded Shimon, Maxim Gavrilov, and Alex Converse. See
 * PROVENANCE.md. SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "synth.h"
#include "etaac_tables.h"

/* Build this translation unit with -ffp-contract=off; do not use FMA. */

typedef struct ETAACComplex {
    float re;
    float im;
} ETAACComplex;

static float etaac_f32(uint32_t u)
{
    union { uint32_t u; float f; } v;
    v.u = u;
    return v.f;
}

#define ETAAC_BF(x, y, a, b) do { (x) = (a) - (b); (y) = (a) + (b); } while (0)

/* Exact float CMUL operation order from tx_priv.h. */
#define ETAAC_CMUL(dre, dim, are, aim, bre, bim) do { \
    (dre) = (are) * (bre) - (aim) * (bim);             \
    (dim) = (are) * (bim) + (aim) * (bre);             \
} while (0)

/* ff_tx_fft_sr_combine's BUTTERFLIES macro, in source order. */
static void etaac_fft_butterflies(ETAACComplex *a0, ETAACComplex *a1,
                                  ETAACComplex *a2, ETAACComplex *a3,
                                  float t1, float t2, float t5, float t6)
{
    float t3, t4, r0, i0, r1, i1;
    r0 = a0->re;
    i0 = a0->im;
    r1 = a1->re;
    i1 = a1->im;
    ETAAC_BF(t3, t5, t5, t1);
    ETAAC_BF(a2->re, a0->re, r0, t5);
    ETAAC_BF(a3->im, a1->im, i1, t3);
    ETAAC_BF(t4, t6, t2, t6);
    ETAAC_BF(a3->re, a1->re, r1, t4);
    ETAAC_BF(a2->im, a0->im, i0, t6);
}

/* ff_tx_fft_sr_combine's TRANSFORM macro, in source order. */
static void etaac_fft_transform(ETAACComplex *a0, ETAACComplex *a1,
                                ETAACComplex *a2, ETAACComplex *a3,
                                float wre, float wim)
{
    float t1, t2, t5, t6;
    ETAAC_CMUL(t1, t2, a2->re, a2->im, wre, -wim);
    ETAAC_CMUL(t5, t6, a3->re, a3->im, wre,  wim);
    etaac_fft_butterflies(a0, a1, a2, a3, t1, t2, t5, t6);
}

/* Copied from ff_tx_fft_sr_combine; table bits are the pinned float table. */
static void etaac_fft_sr_combine(ETAACComplex *z, const uint32_t *cos_bits,
                                 int len)
{
    const int o1 = 2 * len;
    const int o2 = 4 * len;
    const int o3 = 6 * len;
    const uint32_t *wim = cos_bits + o1 - 7;

    for (int i = 0; i < len; i += 4) {
        etaac_fft_transform(&z[0], &z[o1 + 0], &z[o2 + 0], &z[o3 + 0], etaac_f32(cos_bits[0]), etaac_f32(wim[7]));
        etaac_fft_transform(&z[2], &z[o1 + 2], &z[o2 + 2], &z[o3 + 2], etaac_f32(cos_bits[2]), etaac_f32(wim[5]));
        etaac_fft_transform(&z[4], &z[o1 + 4], &z[o2 + 4], &z[o3 + 4], etaac_f32(cos_bits[4]), etaac_f32(wim[3]));
        etaac_fft_transform(&z[6], &z[o1 + 6], &z[o2 + 6], &z[o3 + 6], etaac_f32(cos_bits[6]), etaac_f32(wim[1]));

        etaac_fft_transform(&z[1], &z[o1 + 1], &z[o2 + 1], &z[o3 + 1], etaac_f32(cos_bits[1]), etaac_f32(wim[6]));
        etaac_fft_transform(&z[3], &z[o1 + 3], &z[o2 + 3], &z[o3 + 3], etaac_f32(cos_bits[3]), etaac_f32(wim[4]));
        etaac_fft_transform(&z[5], &z[o1 + 5], &z[o2 + 5], &z[o3 + 5], etaac_f32(cos_bits[5]), etaac_f32(wim[2]));
        etaac_fft_transform(&z[7], &z[o1 + 7], &z[o2 + 7], &z[o3 + 7], etaac_f32(cos_bits[7]), etaac_f32(wim[0]));

        z += 8;
        cos_bits += 8;
        wim -= 8;
    }
}

static void etaac_fft2(ETAACComplex *dst, ETAACComplex *src)
{
    ETAACComplex tmp;
    ETAAC_BF(tmp.re, dst[0].re, src[0].re, src[1].re);
    ETAAC_BF(tmp.im, dst[0].im, src[0].im, src[1].im);
    dst[1] = tmp;
}

static void etaac_fft4(ETAACComplex *dst, ETAACComplex *src)
{
    float t1, t2, t3, t4, t5, t6, t7, t8;
    ETAAC_BF(t3, t1, src[0].re, src[1].re);
    ETAAC_BF(t8, t6, src[3].re, src[2].re);
    ETAAC_BF(dst[2].re, dst[0].re, t1, t6);
    ETAAC_BF(t4, t2, src[0].im, src[1].im);
    ETAAC_BF(t7, t5, src[2].im, src[3].im);
    ETAAC_BF(dst[3].im, dst[1].im, t4, t8);
    ETAAC_BF(dst[3].re, dst[1].re, t3, t7);
    ETAAC_BF(dst[2].im, dst[0].im, t2, t5);
}

static void etaac_fft8(ETAACComplex *dst, ETAACComplex *src)
{
    float t1, t2, t5, t6;
    const float c = etaac_f32(etaac_fft_tab_8[1]);
    etaac_fft4(dst, src);
    ETAAC_BF(t1, dst[5].re, src[4].re, -src[5].re);
    ETAAC_BF(t2, dst[5].im, src[4].im, -src[5].im);
    ETAAC_BF(t5, dst[7].re, src[6].re, -src[7].re);
    ETAAC_BF(t6, dst[7].im, src[6].im, -src[7].im);
    etaac_fft_butterflies(&dst[0], &dst[2], &dst[4], &dst[6], t1, t2, t5, t6);
    etaac_fft_transform(&dst[1], &dst[3], &dst[5], &dst[7], c, c);
}

static void etaac_fft16(ETAACComplex *dst, ETAACComplex *src)
{
    float t1, t2, t5, t6;
    const float c1 = etaac_f32(etaac_fft_tab_16[1]);
    const float c2 = etaac_f32(etaac_fft_tab_16[2]);
    const float c3 = etaac_f32(etaac_fft_tab_16[3]);
    etaac_fft8(dst + 0, src + 0);
    etaac_fft4(dst + 8, src + 8);
    etaac_fft4(dst + 12, src + 12);
    t1 = dst[8].re;
    t2 = dst[8].im;
    t5 = dst[12].re;
    t6 = dst[12].im;
    etaac_fft_butterflies(&dst[0], &dst[4], &dst[8], &dst[12], t1, t2, t5, t6);
    etaac_fft_transform(&dst[2], &dst[6], &dst[10], &dst[14], c2, c2);
    etaac_fft_transform(&dst[1], &dst[5], &dst[9], &dst[13], c1, c3);
    etaac_fft_transform(&dst[3], &dst[7], &dst[11], &dst[15], c3, c1);
}

/* The source declares these fixed split-radix calls. This dispatcher preserves
 * exactly that recursion/order for the two needed FFT lengths. */
static const uint32_t *etaac_fft_tab(unsigned n)
{
    switch (n) {
    case 32:  return etaac_fft_tab_32;
    case 64:  return etaac_fft_tab_64;
    case 128: return etaac_fft_tab_128;
    case 256: return etaac_fft_tab_256;
    default:  return etaac_fft_tab_512;
    }
}

static void etaac_fft(ETAACComplex *dst, ETAACComplex *src, unsigned n)
{
    if (n == 2) {
        etaac_fft2(dst, src);
    } else if (n == 4) {
        etaac_fft4(dst, src);
    } else if (n == 8) {
        etaac_fft8(dst, src);
    } else if (n == 16) {
        etaac_fft16(dst, src);
    } else {
        const unsigned n4 = n >> 2;
        etaac_fft(dst,          src,          n >> 1);
        etaac_fft(dst + n4 * 2, src + n4 * 2, n4);
        etaac_fft(dst + n4 * 3, src + n4 * 3, n4);
        etaac_fft_sr_combine(dst, etaac_fft_tab(n), (int)(n4 >> 1));
    }
}

static int etaac_imdct_inner(float *out, const float *coeff, unsigned n,
                             float *work, const uint32_t *exp_bits,
                             const uint16_t *perm)
{
    const unsigned fft_n = n >> 1;
    ETAACComplex *z = (ETAACComplex *)work;

    for (unsigned i = 0; i < fft_n; i++) {
        const unsigned k = (unsigned)perm[i] << 1;
        const float are = coeff[n - 1 - k];
        const float aim = coeff[k];
        const float bre = etaac_f32(exp_bits[2 * i + 0]);
        const float bim = etaac_f32(exp_bits[2 * i + 1]);
        ETAAC_CMUL(z[i].re, z[i].im, are, aim, bre, bim);
    }

    etaac_fft(z, z, fft_n);

    /* ff_tx_mdct_inv's final rotation uses the unpermuted second half. */
    exp_bits += n;
    for (unsigned i = 0; i < fft_n / 2; i++) {
        const unsigned i0 = fft_n / 2 + i;
        const unsigned i1 = fft_n / 2 - i - 1;
        const float src1re = z[i1].im;
        const float src1im = z[i1].re;
        const float src0re = z[i0].im;
        const float src0im = z[i0].re;
        const float e1im = etaac_f32(exp_bits[2 * i1 + 1]);
        const float e1re = etaac_f32(exp_bits[2 * i1 + 0]);
        const float e0im = etaac_f32(exp_bits[2 * i0 + 1]);
        const float e0re = etaac_f32(exp_bits[2 * i0 + 0]);
        ETAAC_CMUL(z[i1].re, z[i0].im, src1re, src1im, e1im, e1re);
        ETAAC_CMUL(z[i0].re, z[i1].im, src0re, src0im, e0im, e0re);
    }
    for (unsigned i = 0; i < n; i++)
        out[i] = work[i];
    return 0;
}

int etaac_imdct(float *out, const float *coeff, unsigned n, float *scratch)
{
    if (!out || !coeff || !scratch || (n != 128 && n != 1024) ||
        out == scratch || coeff == scratch)
        return -1;
    if (n == 128)
        return etaac_imdct_inner(out, coeff, n, scratch, etaac_mdct_exp_128,
                                 etaac_mdct_perm_128);
    return etaac_imdct_inner(out, coeff, n, scratch, etaac_mdct_exp_1024,
                             etaac_mdct_perm_1024);
}

/* Exact scalar vector_fmul_window_c operation order from float_dsp.c. */
static void etaac_mul_window(float *dst, const float *src0, const float *src1,
                             const uint32_t *win, unsigned len)
{
    dst += len;
    win += len;
    src0 += len;
    for (int i = -(int)len, j = (int)len - 1; i < 0; i++, j--) {
        const float s0 = src0[i];
        const float s1 = src1[j];
        const float wi = etaac_f32(win[i]);
        const float wj = etaac_f32(win[j]);
        dst[i] = s0 * wj - s1 * wi;
        dst[j] = s0 * wi + s1 * wj;
    }
}

static void etaac_copy(float *dst, const float *src, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        dst[i] = src[i];
}

static const uint32_t *etaac_window(unsigned shape, unsigned n)
{
    if (shape == ETAAC_SINE)
        return n == 128 ? etaac_sine_128 : etaac_sine_1024;
    return n == 128 ? etaac_kbd_128 : etaac_kbd_1024;
}

int etaac_synth(float *out1024, float *saved512, const float *coeff1024,
                unsigned sequence, unsigned previous_sequence,
                unsigned shape, unsigned previous_shape, float *scratch)
{
    float *buf;
    float *temp;
    float *imdct_work;
    const uint32_t *lwindow_prev;
    const uint32_t *swindow;
    const uint32_t *swindow_prev;
    const int long_to_long =
        ((previous_sequence == ETAAC_ONLY_LONG || previous_sequence == ETAAC_LONG_STOP) &&
         (sequence == ETAAC_ONLY_LONG || sequence == ETAAC_LONG_START));

    if (!out1024 || !saved512 || !coeff1024 || !scratch ||
        sequence > ETAAC_LONG_STOP || previous_sequence > ETAAC_LONG_STOP ||
        shape > ETAAC_KBD || previous_shape > ETAAC_KBD)
        return -1;

    buf = scratch;
    temp = scratch + 1024;
    imdct_work = scratch + 2048;
    lwindow_prev = etaac_window(previous_shape, 1024);
    swindow = etaac_window(shape, 128);
    swindow_prev = etaac_window(previous_shape, 128);

    if (sequence == ETAAC_EIGHT_SHORT) {
        for (unsigned i = 0; i < 1024; i += 128)
            etaac_imdct(buf + i, coeff1024 + i, 128, imdct_work);
    } else {
        etaac_imdct(buf, coeff1024, 1024, imdct_work);
    }

    if (long_to_long) {
        etaac_mul_window(out1024, saved512, buf, lwindow_prev, 512);
    } else {
        etaac_copy(out1024, saved512, 448);
        if (sequence == ETAAC_EIGHT_SHORT) {
            etaac_mul_window(out1024 + 448 + 0 * 128, saved512 + 448, buf + 0 * 128, swindow_prev, 64);
            etaac_mul_window(out1024 + 448 + 1 * 128, buf + 0 * 128 + 64, buf + 1 * 128, swindow, 64);
            etaac_mul_window(out1024 + 448 + 2 * 128, buf + 1 * 128 + 64, buf + 2 * 128, swindow, 64);
            etaac_mul_window(out1024 + 448 + 3 * 128, buf + 2 * 128 + 64, buf + 3 * 128, swindow, 64);
            etaac_mul_window(temp,                    buf + 3 * 128 + 64, buf + 4 * 128, swindow, 64);
            etaac_copy(out1024 + 448 + 4 * 128, temp, 64);
        } else {
            etaac_mul_window(out1024 + 448, saved512 + 448, buf, swindow_prev, 64);
            etaac_copy(out1024 + 576, buf + 64, 448);
        }
    }

    if (sequence == ETAAC_EIGHT_SHORT) {
        etaac_copy(saved512, temp + 64, 64);
        etaac_mul_window(saved512 + 64,  buf + 4 * 128 + 64, buf + 5 * 128, swindow, 64);
        etaac_mul_window(saved512 + 192, buf + 5 * 128 + 64, buf + 6 * 128, swindow, 64);
        etaac_mul_window(saved512 + 320, buf + 6 * 128 + 64, buf + 7 * 128, swindow, 64);
        etaac_copy(saved512 + 448, buf + 7 * 128 + 64, 64);
    } else if (sequence == ETAAC_LONG_START) {
        etaac_copy(saved512, buf + 512, 448);
        etaac_copy(saved512 + 448, buf + 7 * 128 + 64, 64);
    } else {
        etaac_copy(saved512, buf + 512, 512);
    }
    return 0;
}
