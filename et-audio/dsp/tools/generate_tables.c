/*
 * Host-only table generator derived from FFmpeg n7.1.1:
 * libavutil/{tx_template.c,tx.c,mathematics.c}, libavcodec/{kbdwin.c,
 * sinewin_tablegen.h}.  Output is consumed by freestanding etaac_synth.c.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

static uint32_t fbits(float x)
{
    union { float f; uint32_t u; } v;
    v.f = x;
    return v.u;
}

/* Operation order copied from libavutil/mathematics.c. */
static double eval_poly(const double *coeff, int size, double x)
{
    double sum = coeff[size - 1];
    for (int i = size - 2; i >= 0; --i) {
        sum *= x;
        sum += coeff[i];
    }
    return sum;
}

/* Copied from av_bessel_i0 in the pinned libavutil/mathematics.c. */
static double bessel_i0(double x)
{
    static const double p1[] = {
        -2.2335582639474375249e+15, -5.5050369673018427753e+14,
        -3.2940087627407749166e+13, -8.4925101247114157499e+11,
        -1.1912746104985237192e+10, -1.0313066708737980747e+08,
        -5.9545626019847898221e+05, -2.4125195876041896775e+03,
        -7.0935347449210549190e+00, -1.5453977791786851041e-02,
        -2.5172644670688975051e-05, -3.0517226450451067446e-08,
        -2.6843448573468483278e-11, -1.5982226675653184646e-14,
        -5.2487866627945699800e-18,
    };
    static const double q1[] = {
        -2.2335582639474375245e+15, 7.8858692566751002988e+12,
        -1.2207067397808979846e+10, 1.0377081058062166144e+07,
        -4.8527560179962773045e+03, 1.0,
    };
    static const double p2[] = {
        -2.2210262233306573296e-04, 1.3067392038106924055e-02,
        -4.4700805721174453923e-01, 5.5674518371240761397e+00,
        -2.3517945679239481621e+01, 3.1611322818701131207e+01,
        -9.6090021968656180000e+00,
    };
    static const double q2[] = {
        -5.5194330231005480228e-04, 3.2547697594819615062e-02,
        -1.1151759188741312645e+00, 1.3982595353892851542e+01,
        -6.0228002066743340583e+01, 8.5539563258012929600e+01,
        -3.1446690275135491500e+01, 1.0,
    };
    double y, r, factor;
    if (x == 0)
        return 1.0;
    x = fabs(x);
    if (x <= 15) {
        y = x * x;
        return eval_poly(p1, (int)(sizeof(p1) / sizeof(*p1)), y) /
               eval_poly(q1, (int)(sizeof(q1) / sizeof(*q1)), y);
    }
    y = 1 / x - 1.0 / 15;
    r = eval_poly(p2, (int)(sizeof(p2) / sizeof(*p2)), y) /
        eval_poly(q2, (int)(sizeof(q2) / sizeof(*q2)), y);
    factor = exp(x) / sqrt(x);
    return factor * r;
}

/* Operation order copied from libavcodec/kbdwin.c. */
static void kbd_window(float *window, float alpha, int n)
{
    double temp[1024 / 2 + 1];
    double sum = 0.0, scale = 0.0;
    double alpha2 = 4 * (alpha * M_PI / n) * (alpha * M_PI / n);
    int i;
    for (i = 0; i <= n / 2; i++) {
        double tmp = i * (n - i) * alpha2;
        temp[i] = bessel_i0(sqrt(tmp));
        scale += temp[i] * (1 + (i && i < n / 2));
    }
    scale = 1.0 / (scale + 1);
    for (i = 0; i <= n / 2; i++) {
        sum += temp[i];
        window[i] = sqrt(sum * scale);
    }
    for (; i < n; i++) {
        sum += temp[n - i];
        window[i] = sqrt(sum * scale);
    }
}

/* Copied from split_radix_permutation in the pinned libavutil/tx.c. */
static int split_radix_permutation(int i, int len, int inv)
{
    len >>= 1;
    if (len <= 1)
        return i & 1;
    if (!(i & len))
        return split_radix_permutation(i, len, inv) * 2;
    len >>= 1;
    return split_radix_permutation(i, len, inv) * 4 + 1 - 2 * (!(i & len) ^ inv);
}

static void print_u32(const char *name, const float *v, int n)
{
    printf("static const uint32_t %s[%d] = {\n", name, n);
    for (int i = 0; i < n; i++) {
        if (!(i & 7)) printf("    ");
        printf("UINT32_C(0x%08x)%s", fbits(v[i]), i + 1 == n ? "" : (i & 7) == 7 ? "," : ", ");
        if ((i & 7) == 7 || i + 1 == n) printf("\n");
    }
    printf("};\n\n");
}

static void print_u16(const char *name, const uint16_t *v, int n)
{
    printf("static const uint16_t %s[%d] = {\n", name, n);
    for (int i = 0; i < n; i++) {
        if (!(i & 15)) printf("    ");
        printf("%u%s", (unsigned)v[i], i + 1 == n ? "" : (i & 15) == 15 ? "," : ", ");
        if ((i & 15) == 15 || i + 1 == n) printf("\n");
    }
    printf("};\n\n");
}

static void emit_fft_tab(int n)
{
    float t[129];
    for (int i = 0; i < n / 4; i++)
        t[i] = (float)cos((double)i * 2 * M_PI / n);
    t[n / 4] = 0.0f;
    char name[64];
    snprintf(name, sizeof(name), "etaac_fft_tab_%d", n);
    print_u32(name, t, n / 4 + 1);
}

static void emit_mdct(int n)
{
    const int fft_n = n / 2;
    float e[2048];
    uint16_t p[512];
    /* Float assignment matches AAC init_dsp followed by ff_tx_mdct_gen_exp. */
    float scale_f = (float)((1.0 / n) / 32768.0f);
    double scale = sqrt(fabs((double)scale_f));
    for (int i = 0; i < fft_n; i++)
        p[i] = (uint16_t)(-split_radix_permutation(i, fft_n, 1) & (fft_n - 1));
    for (int i = 0; i < fft_n; i++) {
        const double alpha = M_PI_2 * (i + 1.0 / 8.0) / fft_n;
        e[n + 2 * i + 0] = (float)(cos(alpha) * scale);
        e[n + 2 * i + 1] = (float)(sin(alpha) * scale);
    }
    /* ff_tx_mdct_gen_exp places the pre-permuted exponents first. */
    for (int i = 0; i < fft_n; i++) {
        e[2 * i + 0] = e[n + 2 * p[i] + 0];
        e[2 * i + 1] = e[n + 2 * p[i] + 1];
    }
    char ename[64], pname[64];
    snprintf(ename, sizeof(ename), "etaac_mdct_exp_%d", n);
    snprintf(pname, sizeof(pname), "etaac_mdct_perm_%d", n);
    print_u32(ename, e, 2 * n);
    print_u16(pname, p, fft_n);
}

int main(void)
{
    float sine128[128], sine1024[1024], kbd128[128], kbd1024[1024];
    printf("/* Generated by tools/generate_tables.c; do not edit.\n"
           " * Source: pinned FFmpeg n7.1.1 tx/aac scalar initializers.\n"
           " * SPDX-License-Identifier: LGPL-2.1-or-later */\n"
           "#ifndef ET_AUDIO_DSP_ETAAC_TABLES_H\n#define ET_AUDIO_DSP_ETAAC_TABLES_H\n"
           "#include <stdint.h>\n\n");
    for (int i = 0; i < 128; i++)
        sine128[i] = sinf((i + 0.5) * (M_PI / (2.0 * 128)));
    for (int i = 0; i < 1024; i++)
        sine1024[i] = sinf((i + 0.5) * (M_PI / (2.0 * 1024)));
    kbd_window(kbd128, 6.0f, 128);
    kbd_window(kbd1024, 4.0f, 1024);
    print_u32("etaac_sine_128", sine128, 128);
    print_u32("etaac_sine_1024", sine1024, 1024);
    print_u32("etaac_kbd_128", kbd128, 128);
    print_u32("etaac_kbd_1024", kbd1024, 1024);
    emit_fft_tab(8); emit_fft_tab(16); emit_fft_tab(32); emit_fft_tab(64);
    emit_fft_tab(128); emit_fft_tab(256); emit_fft_tab(512);
    emit_mdct(128); emit_mdct(1024);
    printf("#endif\n");
    return 0;
}
