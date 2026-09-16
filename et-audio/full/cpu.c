/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Native complete AAC-LC packet decoder/oracle for ETAACFullInput ABI 4.
 * This deliberately uses libavcodec directly: no libavformat/demux timing.
 */
#define _POSIX_C_SOURCE 200809L
#include "protocol.h"
#include "libavcodec/avcodec.h"
#include "libavutil/channel_layout.h"
#include "libavutil/cpu.h"
#include "libavutil/error.h"
#include "libavutil/frame.h"
#include "libavutil/samplefmt.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_INPUT_BYTES (64u * 1024u * 1024u)

typedef struct FullInput {
    uint8_t *bytes;
    size_t size;
    uint32_t packets, channels, rate;
    uint64_t *offsets;
    uint32_t *lengths;
} FullInput;

typedef struct Timing {
    double setup_s, decode_s, close_s;
    uint64_t frames, samples, peak_bits, above_one;
    double checksum;
} Timing;

static double now_s(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int fail(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); fputs("input reject: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap); return -1;
}
static void full_input_free(FullInput *in) {
    free(in->offsets); free(in->lengths); free(in->bytes); memset(in, 0, sizeof(*in));
}

static int full_input_load(const char *path, FullInput *in) {
    struct stat st;
    FILE *f = NULL;
    uint32_t capacity;
    const ETAACFullInput *header;
    const ETAACFullPacket *packets;
    memset(in, 0, sizeof(*in));
    if (stat(path, &st) || st.st_size < 0 || (uintmax_t)st.st_size > MAX_INPUT_BYTES) return fail("cannot stat bounded input %s", path);
    in->size = (size_t)st.st_size;
    if (in->size < sizeof(ETAACFullInput) || (in->size & 63)) return fail("file is not a 64-byte-aligned complete header");
    f = fopen(path, "rb"); if (!f) return fail("cannot open %s", path);
    in->bytes = malloc(in->size); if (!in->bytes) { fclose(f); return fail("out of memory reading input"); }
    if (fread(in->bytes, 1, in->size, f) != in->size || fclose(f)) { full_input_free(in); return fail("cannot read input"); }
    /* This is the ABI's single shared strict envelope admission gate.  It
     * validates header/count, exact descriptor offsets/ranges, all padding and
     * the narrow ADTS LC/no-CRC/rate contract before libavcodec is initialized. */
    capacity = le32(in->bytes + 16);
    if (!etaac_full_input_valid(in->bytes, in->size, capacity)) {
        full_input_free(in); return fail("shared ETAAC full envelope validation");
    }
    header = (const ETAACFullInput *)in->bytes;
    packets = (const ETAACFullPacket *)(in->bytes + sizeof(*header));
    in->packets = header->packet_count; in->channels = header->channels; in->rate = header->sample_rate;
    in->offsets = calloc(in->packets, sizeof(*in->offsets)); in->lengths = calloc(in->packets, sizeof(*in->lengths));
    if (!in->offsets || !in->lengths) { full_input_free(in); return fail("out of memory allocating descriptors"); }
    for (uint32_t i = 0; i < in->packets; i++) {
        in->offsets[i] = packets[i].offset;
        in->lengths[i] = packets[i].bytes;
    }
    return 0;
}

static int decode_once(const FullInput *in, float *store, Timing *t) {
    const AVCodec *codec;
    AVCodecContext *ctx = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    int rc = -1;
    double begin = now_s();
    codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (!codec) { fprintf(stderr, "AAC decoder unavailable\n"); goto done; }
    ctx = avcodec_alloc_context3(codec);
    pkt = av_packet_alloc(); frame = av_frame_alloc();
    if (!ctx || !pkt || !frame) { fprintf(stderr, "FFmpeg allocation failure\n"); goto done; }
    /* ADTS carries the AudioSpecificConfig; the driver verifies it independently. */
    if (avcodec_open2(ctx, codec, NULL) < 0) { fprintf(stderr, "avcodec_open2 failed\n"); goto done; }
    t->setup_s += now_s() - begin;
    begin = now_s();
    for (uint32_t i = 0; i < in->packets; i++) {
        int got = 0;
        av_packet_unref(pkt);
        pkt->data = in->bytes + in->offsets[i]; pkt->size = (int)in->lengths[i];
        if (avcodec_send_packet(ctx, pkt) < 0) { fprintf(stderr, "send failed at packet %u\n", i); goto done; }
        for (;;) {
            int ret = avcodec_receive_frame(ctx, frame);
            if (ret == AVERROR(EAGAIN)) break;
            if (ret == AVERROR_EOF) { fprintf(stderr, "early EOF at packet %u\n", i); goto done; }
            if (ret < 0) { fprintf(stderr, "receive failed at packet %u\n", i); goto done; }
            got++;
            if (got != 1 || frame->nb_samples != 1024 || frame->sample_rate != (int)in->rate ||
                frame->ch_layout.nb_channels != (int)in->channels || frame->format != AV_SAMPLE_FMT_FLTP ||
                ctx->profile != AV_PROFILE_AAC_LOW) {
                fprintf(stderr, "decoded shape/profile mismatch at packet %u (frames=%d n=%d rate=%d ch=%d fmt=%d profile=%d)\n", i, got, frame->nb_samples, frame->sample_rate, frame->ch_layout.nb_channels, frame->format, ctx->profile); goto done;
            }
            for (uint32_t ch = 0; ch < in->channels; ch++) {
                const float *src = (const float *)frame->extended_data[ch];
                float *dst = store + ((size_t)i * in->channels + ch) * 1024;
                /* Match the kernel's finite check and fused resident features. */
                for (unsigned j = 0; j < 1024; j++) {
                    uint32_t bits;
                    memcpy(&bits, src + j, sizeof(bits));
                    bits &= UINT32_C(0x7fffffff);
                    if (bits >= UINT32_C(0x7f800000)) { fprintf(stderr, "nonfinite PCM\n"); goto done; }
                    if (bits > t->peak_bits) t->peak_bits = bits;
                    t->above_one += bits > UINT32_C(0x3f800000);
                }
                memcpy(dst, src, 1024 * sizeof(*dst)); /* Full packet-major output store is in the timed loop. */
                t->checksum += dst[(i + ch * 17) & 1023];
            }
            t->frames++; t->samples += (uint64_t)in->channels * 1024;
            av_frame_unref(frame);
        }
        if (got != 1) { fprintf(stderr, "packet %u yielded %d frames (must be exactly one)\n", i, got); goto done; }
    }
    /* There must be no delayed packet/frame: no flush fallback is accepted. */
    if (avcodec_send_packet(ctx, NULL) < 0 || avcodec_receive_frame(ctx, frame) != AVERROR_EOF) {
        fprintf(stderr, "unexpected delayed AAC frame\n"); goto done;
    }
    /* Make every output store observable even in timing-only invocations. */
    __asm__ volatile("" : : "r"(store) : "memory");
    t->decode_s += now_s() - begin;
    rc = 0;
done:
    begin=now_s();
    av_frame_free(&frame); av_packet_free(&pkt); avcodec_free_context(&ctx);
    t->close_s+=now_s()-begin;
    return rc;
}

static int write_new_pcm(const char *path, const float *pcm, size_t floats) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    const uint8_t *p = (const uint8_t *)pcm; size_t n = floats * sizeof(*pcm);
    if (fd < 0) { perror("refusing output (must not already exist)"); return -1; }
    while (n) { ssize_t w = write(fd, p, n); if (w <= 0) { perror("write PCM"); close(fd); return -1; } p += w; n -= (size_t)w; }
    if (close(fd)) { perror("close PCM"); return -1; }
    return 0;
}

int main(int argc, char **argv) {
    const char *input, *pcm = NULL, *mode = "optimized";
    unsigned long repetitions; char *end = NULL; int scalar = 0;
    FullInput in; Timing total = {0}; float *store = NULL;
    size_t floats;
    /* OUTPUT_PCM is optional: INPUT REPEATS [scalar|optimized], or INPUT OUTPUT_PCM REPEATS [scalar|optimized]. */
    if (argc < 3 || argc > 5) { fprintf(stderr, "usage: %s INPUT [OUTPUT_PCM] REPEATS [scalar|optimized]\n", argv[0]); return 2; }
    input = argv[1]; errno = 0;
    if (argc == 3) repetitions = strtoul(argv[2], &end, 10);
    else if (argc == 4 && (!strcmp(argv[3], "scalar") || !strcmp(argv[3], "optimized"))) { repetitions = strtoul(argv[2], &end, 10); mode = argv[3]; }
    else { pcm = argv[2]; repetitions = strtoul(argv[3], &end, 10); if (argc == 5) mode = argv[4]; }
    if (errno || !end || *end || !repetitions || repetitions > 100000 || (strcmp(mode, "scalar") && strcmp(mode, "optimized"))) { fprintf(stderr, "invalid REPEATS or mode\n"); return 2; }
    scalar = !strcmp(mode, "scalar");
    if (pcm && !strcmp(pcm, input)) { fprintf(stderr, "OUTPUT_PCM must differ from INPUT\n"); return 2; }
    if (pcm) { struct stat st; if (!lstat(pcm, &st)) { fprintf(stderr, "OUTPUT_PCM already exists: %s\n", pcm); return 2; } if (errno != ENOENT) { perror("lstat OUTPUT_PCM"); return 2; } }
    if (full_input_load(input, &in)) return 1;
    if ((size_t)in.packets > SIZE_MAX / in.channels / 1024 || (floats = (size_t)in.packets * in.channels * 1024) > SIZE_MAX / sizeof(float)) { full_input_free(&in); return fail("PCM size overflow"); }
    store = malloc(floats * sizeof(*store)); if (!store) { full_input_free(&in); return fail("out of memory allocating PCM store"); }
    if (scalar) av_force_cpu_flags(0);
    for (unsigned long rep = 0; rep < repetitions; rep++) {
        Timing one = {0};
        if (decode_once(&in, store, &one)) { free(store); full_input_free(&in); return 1; }
        total.setup_s += one.setup_s; total.decode_s += one.decode_s; total.frames += one.frames; total.samples += one.samples; total.checksum += one.checksum;
        total.close_s += one.close_s;
        if (one.peak_bits > total.peak_bits) total.peak_bits = one.peak_bits;
        total.above_one += one.above_one;
    }
    if(total.frames!=(uint64_t)repetitions*in.packets || total.samples!=total.frames*in.channels*1024){free(store);full_input_free(&in);return fail("repeat accounting mismatch");}
    if (pcm && write_new_pcm(pcm, store, floats)) { free(store); full_input_free(&in); return 1; }
    printf("{\"type\":\"aac_full_cpu\",\"mode\":\"%s\",\"scalar\":%d,\"cpu_flags\":%d,\"packets\":%u,\"channels\":%u,\"sample_rate\":%u,\"repeats\":%lu,\"setup_table_context_s\":%.9f,\"decode_output_store_s\":%.9f,\"decoder_close_s\":%.9f,\"total_s\":%.9f,\"per_decode_s\":%.9f,\"frames\":%" PRIu64 ",\"samples\":%" PRIu64 ",\"checksum\":%.17g,\"pcm_written\":%s,\"matched_features\":true,\"consumer_peak_bits\":%" PRIu64 ",\"consumer_above_one\":%" PRIu64 "}\n",
           mode, scalar, av_get_cpu_flags(), in.packets, in.channels, in.rate, repetitions, total.setup_s, total.decode_s, total.close_s, total.setup_s + total.decode_s + total.close_s, (total.setup_s + total.decode_s + total.close_s) / repetitions, total.frames, total.samples, total.checksum, pcm ? "true" : "false", total.peak_bits, total.above_one);
    free(store); full_input_free(&in); return 0;
}
