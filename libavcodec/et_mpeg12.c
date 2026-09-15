/*
 * ETSOC-1 MPEG-2 slice reconstruction offload.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "libavutil/mem.h"
#include "et_mpeg12.h"
#include "et_runtime.h"
#include "hwaccel_internal.h"
#include "internal.h"
#include "mpegvideo.h"
#include "mpegutils.h"

#define ET_QUANT_OFFSET 0
#define ET_TABLE_OFFSET (4 * 64 * sizeof(uint16_t))
#define ET_PAYLOAD_OFFSET (ET_TABLE_OFFSET + ET_MPEG2_MAX_SLICES * sizeof(ETSliceDesc))
#define ET_MAX_INPUT (64U * 1024 * 1024)

/* Host frames own only a generation-tagged handle, never a device allocation. */
typedef struct ETPicture {
    uint64_t address;
    uint32_t generation;
    int valid;
} ETPicture;

typedef struct ETMPEGContext {
    ETRuntime *runtime;
    ETFrameParams params;
    uint64_t slots[3];
    uint32_t generations[3];
    uint64_t input;
    size_t input_capacity;
    uint8_t *staging;
    unsigned staging_capacity;
    uint8_t *readback;
    size_t output_size;
    uint32_t used;
    uint32_t sequence;
    uint8_t rows[ET_MPEG2_MAX_SLICES];
    uint64_t shire_mask;
    int harts;
    int failed;
    int slot;
} ETMPEGContext;

static int et_error(AVCodecContext *avctx, int ret, const char *operation)
{
    ETMPEGContext *ctx = avctx->internal->hwaccel_priv_data;
    ctx->failed = 1;
    av_log(avctx, AV_LOG_ERROR, "ET %s failed: %s (%d); no CPU fallback\n",
           operation, ff_et_runtime_error(ctx->runtime), ret);
    return ret < 0 ? ret : AVERROR_EXTERNAL;
}

static int et_invalid(AVCodecContext *avctx, const char *reason)
{
    ETMPEGContext *ctx = avctx->internal->hwaccel_priv_data;
    ctx->failed = 1;
    av_log(avctx, AV_LOG_ERROR, "ET: %s; no CPU fallback\n", reason);
    return AVERROR_INVALIDDATA;
}

int ff_et_mpeg2_probe(AVCodecContext *avctx)
{
    ETRuntime *runtime = NULL;
    char error[512] = { 0 };
    int ret = ff_et_runtime_open(&runtime, NULL, error, sizeof(error));
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "ET probe init failed: %s\n", error);
        return ret;
    }
    av_log(avctx, AV_LOG_INFO, "ET init OK; explicit probe mode, CPU reconstruction\n");
    ff_et_runtime_close(&runtime);
    return 0;
}

static int et_uninit(AVCodecContext *avctx)
{
    ETMPEGContext *ctx = avctx->internal->hwaccel_priv_data;
    if (!ctx)
        return 0;
    ff_et_runtime_close(&ctx->runtime);
    av_freep(&ctx->staging);
    av_freep(&ctx->readback);
    return 0;
}

static int et_init(AVCodecContext *avctx)
{
    ETMPEGContext *ctx = avctx->internal->hwaccel_priv_data;
    MpegEncContext *s = avctx->priv_data;
    const char *kernel = getenv("FF_ET_KERNEL");
    const char *mask = getenv("FF_ET_SHIRE_MASK");
    char error[512] = { 0 }, *end;
    int64_t shires = 1, harts = 64;
    int ret;

    if (avctx->sw_pix_fmt != AV_PIX_FMT_YUV420P || s->chroma_format != 1 ||
        avctx->lowres || avctx->skip_top || avctx->skip_bottom ||
        (avctx->flags & AV_CODEC_FLAG_GRAY))
        return et_invalid(avctx, "only full-resolution 4:2:0 decoding is supported");
    if (avctx->profile != AV_PROFILE_MPEG2_MAIN && avctx->profile != AV_PROFILE_MPEG2_SIMPLE)
        return et_invalid(avctx, "only MPEG-2 Main and Simple profiles are supported");
    if (avctx->idct_algo != FF_IDCT_AUTO && avctx->idct_algo != FF_IDCT_SIMPLE)
        return et_invalid(avctx, "ET uses the scalar simple IDCT (auto or simple required)");
    av_opt_get_int(avctx->priv_data, "et_shires", 0, &shires);
    av_opt_get_int(avctx->priv_data, "et_harts", 0, &harts);
    if (shires != 1)
        return et_invalid(avctx, "multi-shire frame scheduling is not implemented (-et_shires 1 required)");
    if (harts != 1 && harts != 64)
        return et_invalid(avctx, "-et_harts must be 1 or 64");
    ctx->harts = harts;
    ctx->shire_mask = 1;
    if (mask) {
        errno = 0;
        ctx->shire_mask = strtoull(mask, &end, 0);
        if (errno || !*mask || *end || !ctx->shire_mask ||
            (ctx->shire_mask & (ctx->shire_mask - 1)))
            return et_invalid(avctx, "FF_ET_SHIRE_MASK must select exactly one shire");
    }
    if (!kernel || !*kernel) {
        av_log(avctx, AV_LOG_ERROR, "ET: set FF_ET_KERNEL to mpeg2_slice.elf\n");
        return AVERROR(EINVAL);
    }
    ret = ff_et_runtime_open(&ctx->runtime, kernel, error, sizeof(error));
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "ET init failed: %s\n", error);
        return ret;
    }
    if (ctx->shire_mask & ~ff_et_runtime_shire_mask(ctx->runtime))
        return et_invalid(avctx, "requested shire is unavailable");
    avctx->err_recognition |= AV_EF_EXPLODE;
    av_log(avctx, AV_LOG_INFO, "ET init: shire mask 0x%llx, %d harts, host yuv420p frames\n",
           (unsigned long long)ctx->shire_mask, ctx->harts);
    return 0;
}

static uint64_t et_reference(ETMPEGContext *ctx, const MPVPicture *pic)
{
    const ETPicture *ref;
    int i;
    if (!pic || pic->dummy || !pic->hwaccel_picture_private)
        return 0;
    ref = pic->hwaccel_picture_private;
    if (!ref->valid)
        return 0;
    for (i = 0; i < 3; i++)
        if (ref->address == ctx->slots[i] && ref->generation == ctx->generations[i])
            return ref->address;
    return 0;
}

int ff_et_mpeg2_picture_valid(AVCodecContext *avctx, const MPVPicture *pic)
{
    ETMPEGContext *ctx = avctx->internal->hwaccel_priv_data;
    return ctx && et_reference(ctx, pic) != 0;
}

static int et_alloc_frames(AVCodecContext *avctx)
{
    ETMPEGContext *ctx = avctx->internal->hwaccel_priv_data;
    ETFrameParams *p = &ctx->params;
    int i, ret;
    ctx->output_size = p->frame_bytes + p->mb_height * sizeof(ETSliceStatus);
    ctx->readback = av_mallocz(ctx->output_size);
    if (!ctx->readback)
        return AVERROR(ENOMEM);
    for (i = 0; i < 3; i++) {
        ret = ff_et_runtime_alloc(ctx->runtime, ctx->output_size, &ctx->slots[i]);
        if (ret < 0)
            return et_error(avctx, ret, "frame allocation");
        /* Initial poison/generation zero. No reference uploads after setup. */
        ret = ff_et_runtime_write(ctx->runtime, ctx->slots[i], ctx->readback, ctx->output_size);
        if (ret < 0)
            return et_error(avctx, ret, "frame initialization");
    }
    return 0;
}

static int et_start_frame(AVCodecContext *avctx, const uint8_t *buffer, uint32_t size)
{
    ETMPEGContext *ctx = avctx->internal->hwaccel_priv_data;
    MpegEncContext *s = avctx->priv_data;
    ETFrameParams *p = &ctx->params;
    ETPicture *pic = s->cur_pic.ptr->hwaccel_picture_private;
    uint64_t last = et_reference(ctx, s->last_pic.ptr);
    uint64_t next = et_reference(ctx, s->next_pic.ptr);
    uint16_t *matrices;
    int i, j, ret;

    if (ctx->failed)
        return AVERROR_EXTERNAL;
    if (s->codec_id != AV_CODEC_ID_MPEG2VIDEO || s->chroma_format != 1 ||
        (avctx->profile != AV_PROFILE_MPEG2_MAIN && avctx->profile != AV_PROFILE_MPEG2_SIMPLE))
        return et_invalid(avctx, "only MPEG-2 Main/Simple 4:2:0 bitstreams are supported");
    if (s->picture_structure != PICT_FRAME || !s->progressive_frame ||
        !s->frame_pred_frame_dct || s->concealment_motion_vectors)
        return et_invalid(avctx, "only progressive frame pictures without concealment MVs are supported");
    if (s->width < 2 || s->height < 2 || (s->width & 1) || (s->height & 1) ||
        s->width > 4096 || s->height > 2800 || s->mb_height > ET_MPEG2_MAX_SLICES)
        return et_invalid(avctx, "unsupported coded dimensions");
    if (s->pict_type < AV_PICTURE_TYPE_I || s->pict_type > AV_PICTURE_TYPE_B)
        return et_invalid(avctx, "unsupported picture type");
    if ((s->pict_type != AV_PICTURE_TYPE_I && !last) ||
        (s->pict_type == AV_PICTURE_TYPE_B && !next))
        return et_invalid(avctx, "missing device reference picture");
    if (ctx->slots[0] && (p->width != s->width || p->height != s->height))
        return et_invalid(avctx, "dimensions changed without decoder reinitialization");

    memset(p, 0, sizeof(*p));
    p->abi_version = ET_MPEG2_ABI_VERSION;
    p->width = s->width;
    p->height = s->height;
    p->mb_width = s->mb_width;
    p->mb_height = s->mb_height;
    p->linesize_y = FFALIGN(s->mb_width * 16, ET_CACHE_LINE);
    p->linesize_uv = FFALIGN(s->mb_width * 8, ET_CACHE_LINE);
    p->plane_offset[1] = p->linesize_y * s->mb_height * 16;
    p->plane_offset[2] = p->plane_offset[1] + p->linesize_uv * s->mb_height * 8;
    p->frame_bytes = p->plane_offset[2] + p->linesize_uv * s->mb_height * 8;
    p->pict_type = s->pict_type;
    p->picture_structure = s->picture_structure;
    p->q_scale_type = s->q_scale_type;
    p->intra_dc_precision = s->intra_dc_precision;
    p->intra_vlc_format = s->intra_vlc_format;
    p->alternate_scan = s->alternate_scan;
    p->concealment_mv = s->concealment_motion_vectors;
    p->frame_pred_frame_dct = s->frame_pred_frame_dct;
    p->top_field_first = s->top_field_first;
    p->progressive_frame = s->progressive_frame;
    for (i = 0; i < 2; i++)
        for (j = 0; j < 2; j++)
            p->f_code[i][j] = s->mpeg_f_code[i][j];
    p->active_harts = ctx->harts;
    p->quant_offset = ET_QUANT_OFFSET;
    p->slice_table_offset = ET_TABLE_OFFSET;
    p->bitstream_offset = ET_PAYLOAD_OFFSET;
    if (++ctx->sequence == 0)
        return et_invalid(avctx, "frame generation exhausted; reopen decoder");
    p->frame_id = ctx->sequence;
    if (!ctx->slots[0] && (ret = et_alloc_frames(avctx)) < 0)
        return ret;
    for (ctx->slot = 0; ctx->slot < 3; ctx->slot++)
        if (ctx->slots[ctx->slot] != last && ctx->slots[ctx->slot] != next)
            break;
    av_assert0(ctx->slot < 3);
    p->dst_addr = ctx->slots[ctx->slot];
    p->status_addr = p->dst_addr + p->frame_bytes;
    p->ref_fwd_addr = s->pict_type != AV_PICTURE_TYPE_I ? last : 0;
    p->ref_bwd_addr = s->pict_type == AV_PICTURE_TYPE_B ? next : 0;
    ctx->generations[ctx->slot] = p->frame_id;
    pic->address = p->dst_addr;
    pic->generation = p->frame_id;
    pic->valid = 0;
    ctx->used = ET_PAYLOAD_OFFSET;
    memset(ctx->rows, 0, sizeof(ctx->rows));
    av_fast_malloc(&ctx->staging, &ctx->staging_capacity, ctx->used);
    if (!ctx->staging)
        return AVERROR(ENOMEM);
    memset(ctx->staging, 0, ctx->used);
    matrices = (uint16_t *)(ctx->staging + ET_QUANT_OFFSET);
    for (i = 0; i < 64; i++) {
        int n = s->idsp.idct_permutation[i];
        matrices[i]       = s->intra_matrix[n];
        matrices[64 + i]  = s->inter_matrix[n];
        matrices[128 + i] = s->chroma_intra_matrix[n];
        matrices[192 + i] = s->chroma_inter_matrix[n];
    }
    return 0;
}

static int et_decode_slice(AVCodecContext *avctx, const uint8_t *buffer, uint32_t size)
{
    ETMPEGContext *ctx = avctx->internal->hwaccel_priv_data;
    MpegEncContext *s = avctx->priv_data;
    ETFrameParams *p = &ctx->params;
    ETSliceDesc *desc;
    uint32_t padded;
    if (ctx->failed)
        return AVERROR_EXTERNAL;
    if (s->mb_y < 0 || s->mb_y >= p->mb_height || s->mb_x ||
        ctx->rows[s->mb_y] || p->nb_slices >= p->mb_height)
        return et_invalid(avctx, "expected one full, unique slice per macroblock row");
    if (size < 5 || size > ET_MAX_INPUT - ET_CACHE_LINE ||
        buffer[0] || buffer[1] || buffer[2] != 1 || buffer[3] != s->mb_y + 1)
        return et_invalid(avctx, "invalid slice start code or size");
    padded = FFALIGN(size + ET_CACHE_LINE, ET_CACHE_LINE);
    if (ctx->used > ET_MAX_INPUT - padded)
        return et_invalid(avctx, "frame bitstream staging limit exceeded");
    {
        void *tmp = av_fast_realloc(ctx->staging, &ctx->staging_capacity, ctx->used + padded);
        if (!tmp)
            return AVERROR(ENOMEM);
        ctx->staging = tmp;
    }
    desc = (ETSliceDesc *)(ctx->staging + ET_TABLE_OFFSET) + p->nb_slices;
    desc->bitstream_off = ctx->used - ET_PAYLOAD_OFFSET;
    desc->bitstream_len = size;
    desc->mb_y = s->mb_y;
    desc->quant_scale = s->qscale;
    memcpy(ctx->staging + ctx->used, buffer, size);
    memset(ctx->staging + ctx->used + size, 0, padded - size);
    ctx->used += padded;
    ctx->rows[s->mb_y] = 1;
    p->nb_slices++;
    return 0;
}

static void et_dump_frame(AVCodecContext *avctx)
{
    ETMPEGContext *ctx = avctx->internal->hwaccel_priv_data;
    const ETFrameParams *p = &ctx->params;
    const char *enabled = getenv("FF_ET_DUMP_MB");
    const char *dir = getenv("FF_ET_DUMP_DIR");
    char path[1024];
    FILE *f;
    int plane, y;
    if (!enabled || strcmp(enabled, "1"))
        return;
    if (!dir) dir = ".";
    snprintf(path, sizeof(path), "%s/et-frame-%08u.yuv", dir, p->frame_id);
    f = fopen(path, "wb");
    if (!f) {
        av_log(avctx, AV_LOG_WARNING, "ET: cannot open debug dump %s\n", path);
        return;
    }
    for (plane = 0; plane < 3; plane++) {
        int width = p->width >> !!plane, height = p->height >> !!plane;
        int stride = plane ? p->linesize_uv : p->linesize_y;
        for (y = 0; y < height; y++)
            if (fwrite(ctx->readback + p->plane_offset[plane] + y * stride, 1, width, f) != width)
                av_log(avctx, AV_LOG_WARNING, "ET: short debug dump write\n");
    }
    fclose(f);
    av_log(avctx, AV_LOG_INFO, "ET MB debug: frame_id=%u type=%d %ux%u file=%s (decode order)\n",
           p->frame_id, p->pict_type, p->width, p->height, path);
}

static int et_end_frame(AVCodecContext *avctx)
{
    ETMPEGContext *ctx = avctx->internal->hwaccel_priv_data;
    MpegEncContext *s = avctx->priv_data;
    ETFrameParams *p = &ctx->params;
    ETPicture *pic = s->cur_pic.ptr->hwaccel_picture_private;
    const ETSliceStatus *status;
    uint64_t cycles = 0;
    int i, plane, y, ret;
    if (ctx->failed)
        return AVERROR_EXTERNAL;
    if (p->nb_slices != p->mb_height)
        return et_invalid(avctx, "missing macroblock rows");
    if (ctx->input_capacity < ctx->used) {
        if (ctx->input && (ret = ff_et_runtime_free(ctx->runtime, ctx->input)) < 0)
            return et_error(avctx, ret, "input free");
        ctx->input = 0;
        ctx->input_capacity = 0;
        ret = ff_et_runtime_alloc(ctx->runtime, ctx->used, &ctx->input);
        if (ret < 0)
            return et_error(avctx, ret, "input allocation");
        ctx->input_capacity = ctx->used;
    }
    p->input_addr = ctx->input;
    p->input_bytes = ctx->used;
    ret = ff_et_runtime_write(ctx->runtime, ctx->input, ctx->staging, ctx->used);
    if (ret < 0)
        return et_error(avctx, ret, "frame upload");
    ret = ff_et_runtime_launch(ctx->runtime, p, ctx->shire_mask);
    if (ret < 0)
        return et_error(avctx, ret, "kernel launch");
    ret = ff_et_runtime_read(ctx->runtime, ctx->readback, p->dst_addr, ctx->output_size);
    if (ret < 0)
        return et_error(avctx, ret, "frame readback");
    et_dump_frame(avctx);
    status = (const ETSliceStatus *)(ctx->readback + p->frame_bytes);
    for (i = 0; i < p->nb_slices; i++) {
        if (status[i].frame_id != p->frame_id || status[i].code != ET_DECODE_OK ||
            status[i].mb_decoded != p->mb_width) {
            av_log(avctx, AV_LOG_ERROR, "ET slice %d: status=%u generation=%u/%u MBs=%u/%u bits=%u\n",
                   i, status[i].code, status[i].frame_id, p->frame_id,
                   status[i].mb_decoded, p->mb_width, status[i].bits_consumed);
            return et_invalid(avctx, "device slice reconstruction failed");
        }
        cycles += status[i].cycles;
    }
    for (plane = 0; plane < 3; plane++) {
        int width = p->width >> !!plane, height = p->height >> !!plane;
        int stride = plane ? p->linesize_uv : p->linesize_y;
        for (y = 0; y < height; y++)
            memcpy(s->cur_pic.ptr->f->data[plane] + y * s->cur_pic.ptr->f->linesize[plane],
                   ctx->readback + p->plane_offset[plane] + y * stride, width);
    }
    /* FFmpeg's software ER callbacks skip hwaccels. Clear the initial
     * missing-MB count only after all device rows have been validated. */
    atomic_store(&s->er.error_count, 0);
    pic->valid = 1;
    av_log(avctx, AV_LOG_VERBOSE, "ET frame %u: type=%d slices=%u harts=%u sum_slice_cycles=%llu\n",
           p->frame_id, p->pict_type, p->nb_slices, p->active_harts, (unsigned long long)cycles);
    return 0;
}

const FFHWAccel ff_mpeg2_et_hwaccel = {
    .p.name = "mpeg2_et",
    .p.type = AVMEDIA_TYPE_VIDEO,
    .p.id = AV_CODEC_ID_MPEG2VIDEO,
    .p.pix_fmt = AV_PIX_FMT_YUV420P,
    .start_frame = et_start_frame,
    .decode_slice = et_decode_slice,
    .end_frame = et_end_frame,
    .init = et_init,
    .uninit = et_uninit,
    .priv_data_size = sizeof(ETMPEGContext),
    .frame_priv_data_size = sizeof(ETPicture),
};
