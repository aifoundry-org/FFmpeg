/* SPDX-License-Identifier: LGPL-2.1-or-later
 * HOST-NATIVE TEST ONLY: continue receiving/draining after runtime failures.
 * This deliberately bypasses ffmpeg -xerror, which stops before exercising drain.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libavcodec/avcodec.h"
#include "libavutil/error.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixfmt.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "DRAIN TEST FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define MAX_PACKETS 8
struct Packets { AVPacket *p[MAX_PACKETS]; int n; };
struct Picture { uint64_t hash; enum AVPictureType type; };
struct Results { struct Picture p[MAX_PACKETS]; int n, errors, before_drain; };

static void add_packet(struct Packets *p, const uint8_t *data, int size)
{
    CHECK(p->n < MAX_PACKETS);
    p->p[p->n] = av_packet_alloc(); CHECK(p->p[p->n]);
    CHECK(av_new_packet(p->p[p->n], size) == 0);
    memcpy(p->p[p->n]->data, data, size);
    p->p[p->n]->pts = p->p[p->n]->dts = p->n;
    p->n++;
}
static struct Packets load_packets(const char *path, int whole_packet)
{
    FILE *file = fopen(path, "rb"); long bytes; uint8_t *data;
    struct Packets result = { { 0 }, 0 };
    CHECK(file); CHECK(!fseek(file, 0, SEEK_END)); bytes = ftell(file);
    CHECK(bytes > 0 && bytes < 10000000); rewind(file);
    data = av_mallocz(bytes + AV_INPUT_BUFFER_PADDING_SIZE); CHECK(data);
    CHECK(fread(data, 1, bytes, file) == (size_t)bytes); fclose(file);
    if (whole_packet) {
        add_packet(&result, data, bytes);
    } else {
        AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
        AVCodecContext *ctx = avcodec_alloc_context3(NULL);
        uint8_t *out; int out_size; long offset = 0;
        CHECK(parser && ctx);
        while (offset < bytes) {
            int used = av_parser_parse2(parser, ctx, &out, &out_size,
                                       data + offset, bytes - offset,
                                       AV_NOPTS_VALUE, AV_NOPTS_VALUE, offset);
            CHECK(used >= 0 && (used || out_size)); offset += used;
            if (out_size) add_packet(&result, out, out_size);
        }
        CHECK(av_parser_parse2(parser, ctx, &out, &out_size, NULL, 0,
                              AV_NOPTS_VALUE, AV_NOPTS_VALUE, offset) >= 0);
        if (out_size) add_packet(&result, out, out_size);
        av_parser_close(parser); avcodec_free_context(&ctx);
    }
    av_free(data); return result;
}
static uint64_t fingerprint(const AVFrame *frame)
{
    uint64_t value = UINT64_C(1469598103934665603);
    CHECK(frame->format == AV_PIX_FMT_YUV420P);
    for (int plane = 0; plane < 3; plane++) {
        int width = frame->width >> !!plane, height = frame->height >> !!plane;
        for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) {
            value ^= frame->data[plane][y * frame->linesize[plane] + x];
            value *= UINT64_C(1099511628211);
        }
    }
    return value;
}
static void receive_all(AVCodecContext *ctx, AVFrame *frame, struct Results *out)
{
    for (int attempt = 0; attempt < 16; attempt++) {
        int rc = avcodec_receive_frame(ctx, frame);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return;
        if (rc < 0) { out->errors++; continue; }
        CHECK(out->n < MAX_PACKETS);
        out->p[out->n].type = frame->pict_type;
        out->p[out->n].hash = fingerprint(frame);
        out->n++; av_frame_unref(frame);
    }
    CHECK(!"receive loop did not terminate");
}
static struct Results decode(const struct Packets *packets, const char *fault, int harts)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    AVCodecContext *ctx; AVFrame *frame; struct Results result = { { { 0 } }, 0, 0, 0 };
    CHECK(codec); ctx = avcodec_alloc_context3(codec); frame = av_frame_alloc(); CHECK(ctx && frame);
    if (fault) CHECK(!setenv("FF_ET_TEST_FAIL", fault, 1)); else unsetenv("FF_ET_TEST_FAIL");
    CHECK(!setenv("FF_ET_KERNEL", "NATIVE_TEST_NO_DEVICE_ELF", 1));
    ctx->thread_count = 1; ctx->thread_type = 0;
    ctx->err_recognition = AV_EF_EXPLODE; ctx->idct_algo = FF_IDCT_SIMPLE;
    CHECK(av_opt_set_int(ctx->priv_data, "et", 1, 0) == 0);
    CHECK(av_opt_set_int(ctx->priv_data, "et_harts", harts, 0) == 0);
    CHECK(avcodec_open2(ctx, codec, NULL) == 0);
    for (int i = 0; i < packets->n; i++) {
        int rc = avcodec_send_packet(ctx, packets->p[i]);
        if (rc == AVERROR(EAGAIN)) {
            receive_all(ctx, frame, &result);
            rc = avcodec_send_packet(ctx, packets->p[i]);
        }
        if (rc < 0) result.errors++;
        /* Keep going even after errors: application code can still receive or
         * submit another packet. A failed anchor must never become output. */
        receive_all(ctx, frame, &result);
    }
    result.before_drain = result.n;
    /* Repeated drain/receive calls catch both delayed-picture and EOF leaks. */
    for (int attempt = 0; attempt < 4; attempt++) {
        int rc = avcodec_send_packet(ctx, NULL);
        if (rc < 0 && rc != AVERROR_EOF && rc != AVERROR(EAGAIN)) result.errors++;
        receive_all(ctx, frame, &result);
    }
    av_frame_free(&frame); avcodec_free_context(&ctx); unsetenv("FF_ET_TEST_FAIL");
    return result;
}
static void test_fault(const char *name, const struct Packets *packets,
                       const struct Results *golden, const char *fault, int completed, int harts)
{
    struct Results result = decode(packets, fault, harts);
    CHECK(result.errors > 0);
    CHECK(result.n <= completed);
    for (int i = 0; i < result.n; i++) {
        CHECK(result.p[i].type == golden->p[i].type);
        CHECK(result.p[i].hash == golden->p[i].hash);
    }
    printf("PASS NATIVE drain: %s fault=%s harts=%d returned=%d valid-prefix<=%d\n",
           name, fault, harts, result.n, completed);
}
int main(int argc, char **argv)
{
    struct Packets single, ip;
    CHECK(argc == 3);
    av_log_set_level(AV_LOG_VERBOSE);
    unsetenv("FF_ET_SHIRE_MASK"); unsetenv("FF_ET_SYSEMU"); unsetenv("FF_ET_MEM_CHECK");
    single = load_packets(argv[1], 1);  /* a complete one-I file is one packet */
    ip = load_packets(argv[2], 0);
    CHECK(single.n == 1 && ip.n == 2);
    for (int harts = 1; harts <= 64; harts += 63) {
        struct Results a = decode(&single, NULL, harts), b = decode(&ip, NULL, harts);
        CHECK(a.errors == 0 && a.n == 1 && a.before_drain == 0 && a.p[0].type == AV_PICTURE_TYPE_I);
        CHECK(b.errors == 0 && b.n == 2 && b.before_drain == 1 && b.p[0].type == AV_PICTURE_TYPE_I && b.p[1].type == AV_PICTURE_TYPE_P);
        printf("PASS NATIVE drain: valid whole-I and parsed-I/P controls harts=%d\n", harts);
        test_fault("single-I", &single, &a, "launch", 0, harts);
        test_fault("single-I", &single, &a, "read", 0, harts);
        test_fault("failed-I then P", &ip, &b, "launch:1", 0, harts);
        test_fault("failed-I then P", &ip, &b, "read:1", 0, harts);
        test_fault("valid-I failed-P", &ip, &b, "launch:2", 1, harts);
        test_fault("valid-I failed-P", &ip, &b, "read:2", 1, harts);
    }
    for (int i = 0; i < single.n; i++) av_packet_free(&single.p[i]);
    for (int i = 0; i < ip.n; i++) av_packet_free(&ip.p[i]);
    puts("PASS: NATIVE API drain audit: 4 controls + 12 fault scenarios; no failed I/P output leaked");
    return 0;
}
