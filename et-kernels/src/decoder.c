/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Standalone, bounded MPEG-2 slice decoder. FFmpeg's get_bits/VLC and scalar
 * simple_idct are compiled unchanged. The intra coefficient decoder is
 * reproducibly extracted by scripts/generate.py (see generated/block_intra.h).
 */
#include "config.h"
#include "et_mpeg2.h"
#include "decoder_internal.h"
#include "libavcodec/get_bits.h"
#include "libavcodec/simple_idct.h"
#include "cache.h"
#include "motion.h"
#include "reconstruct.h"
#ifndef ET_PREFETCH_MC
#define ET_PREFETCH_MC 0
#endif
#ifndef ET_EVEN_ONLY
#define ET_EVEN_ONLY 0
#endif
#ifndef ET_SPREAD_HARTS
#define ET_SPREAD_HARTS 0
#endif
#include "tables.h"

#if HAVE_FAST_UNALIGNED
#error ET kernels require HAVE_FAST_UNALIGNED=0
#endif

#define TEX_VLC_BITS 9
#define MAX_INDEX 63
#ifndef ET_PREQUANT
#define ET_PREQUANT 0
#endif
#if ET_PREQUANT
#define ET_COEFF_QSCALE(s) 1
#else
#define ET_COEFF_QSCALE(s) ((s)->qscale)
#endif

typedef struct ETBlockState {
    GetBitContext gb;
    struct { const uint8_t *permutated; } intra_scantable;
    const uint16_t *intra_matrix, *chroma_intra_matrix;
    const uint16_t *inter_matrix, *chroma_inter_matrix;
    int qscale, last_dc[3], intra_dc_precision, intra_vlc_format, valid_bits;
#if ET_PREQUANT
    const uint16_t *raw_quant;
    _Alignas(32) uint16_t scaled_quant[256];
#endif
} ETBlockState;

static void et_set_qscale(ETBlockState *s, int qscale)
{
#if ET_PREQUANT
    if (qscale != s->qscale) {
        /* Max 112*255=28560: exact uint16 product, no saturation/narrowing.
         * One scale change replaces a multiplication for every coefficient. */
#ifdef ET_DEVICE
        static const _Alignas(32) uint32_t offsets[8] = {0,2,4,6,8,10,12,14};
        const uint16_t *src = s->raw_quant;
        uint16_t *dst = s->scaled_quant;
        uintptr_t masks, count = 32;
        __asm__ volatile(
            "mova.x.m %[mask]\n" "mov.m.x m0, zero, 255\n"
            "flw.ps f2, 0(%[offsets])\n" "fbcx.ps f1, %[q]\n"
            "1:\n" "fgh.ps f0, f2(%[src])\n" "fmul.pi f0, f0, f1\n"
            "fsch.ps f0, f2(%[dst])\n"
            "addi %[src], %[src], 16\n" "addi %[dst], %[dst], 16\n"
            "addi %[count], %[count], -1\n" "bnez %[count], 1b\n"
            "mova.m.x %[mask]\n"
            : [src] "+&r"(src), [dst] "+&r"(dst), [count] "+&r"(count), [mask] "=&r"(masks)
            : [offsets] "r"(offsets), [q] "r"(qscale)
            : "f0", "f1", "f2", "memory");
#else
        for (unsigned i=0; i<256; i++) s->scaled_quant[i] = s->raw_quant[i] * qscale;
#endif
    }
#endif
    s->qscale = qscale;
}

static int et_decode_dc(GetBitContext *gb, int component)
{
    int code = get_vlc2(gb, component ? ff_dc_chroma_vlc : ff_dc_lum_vlc, 9, 2);
    if (code < 0 || code > 11) return INT_MIN;
    return code ? get_xbits(gb, code) : 0;
}
#include "block_intra.h"
#include "block_inter.h"

#ifndef ET_CACHED_BITS
#define ET_CACHED_BITS 1
#endif
#ifndef ET_DIRECT_BITS
#define ET_DIRECT_BITS 0
#endif
#ifndef ET_FAST_HEADERS
#define ET_FAST_HEADERS 1
#endif
#ifndef ET_BIT_WINDOW_BYTES
#define ET_BIT_WINDOW_BYTES 1024
#endif
_Static_assert(ET_BIT_WINDOW_BYTES >= 256 && !(ET_BIT_WINDOW_BYTES & 7), "bit window alignment/size");

typedef struct ETBits {
    const uint8_t *data;
    uint32_t bytes, pos;
    int error;
#if ET_CACHED_BITS
    uint32_t window_base, window_bytes;
    _Alignas(8) uint8_t window[ET_BIT_WINDOW_BYTES + 64];
#endif
} ETBits;

/* FFmpeg's readers assume padded input. The ABI deliberately does not: a
 * guarded local window supplies that padding without ever overreading DRAM.
 * A block consumes <= 20 + 63*24 + 16 bits, hence a 256-byte window suffices.
 * All copies remain bytewise even with compiler vectorization enabled. */
#if ET_CACHED_BITS
static av_always_inline void bit_refill(ETBits *b, unsigned n)
{
    uint32_t off = b->pos >> 3;
    /* Copy each slice region once, rather than copying/zeroing 320 bytes per
     * coefficient block and 72 bytes per header read. The public ABI still
     * needs NO speculative-read padding; only this owned cache is padded.
     * Refills start on an eight-byte slice offset, preserving aligned host
     * slice bases and permitting wide memcpy without unaligned DRAM loads. */
    uint32_t end = b->window_base + b->window_bytes;
    if (off < b->window_base || off > end ||
        (n > end - off && end != b->bytes)) {
        b->window_base = off & ~7u;
        uint32_t count = b->bytes - b->window_base;
        if (count > ET_BIT_WINDOW_BYTES) count = ET_BIT_WINDOW_BYTES;
        memcpy(b->window, b->data + b->window_base, count);
        memset(b->window + count, 0, 64);
        b->window_bytes = count;
    }
}
#endif

static unsigned bit_window(ETBits *b, uint8_t *buf, unsigned n, GetBitContext *gb)
{
#if ET_DIRECT_BITS
    /* An interior window may use original input directly when the entire
     * reader envelope, INCLUDING 64 bytes of lookahead, is inside the slice.
     * This never relies on ABI padding and never rounds a pointer down. */
    uint32_t byte = b->pos >> 3;
    if (b->bytes - byte >= n + 64) {
        init_get_bits(gb, b->data + byte, n * 8);
        skip_bits(gb, b->pos & 7);
        return n * 8;
    }
#endif
#if ET_CACHED_BITS
    bit_refill(b, n);
    init_get_bits(gb, b->window, b->window_bytes * 8);
    skip_bits(gb, b->pos - b->window_base * 8);
    return b->window_bytes * 8;
#else
    uint32_t off = b->pos >> 3;
    const volatile uint8_t *src = b->data;
    for (unsigned i = 0; i < n; i++)
        buf[i] = off < b->bytes && i < b->bytes - off ? src[off+i] : 0;
    /* get_bits' maximum speculative access is 8 bytes. */
    for (unsigned i = n; i < n + 64; i++) buf[i] = 0;
    init_get_bits(gb, buf, n * 8);
    skip_bits(gb, b->pos & 7);
    unsigned valid = (b->bytes - off) * 8;
    return valid < n * 8 ? valid : n * 8;
#endif
}

static unsigned read_bits(ETBits *b, unsigned n)
{
    if (b->error || n > 24 || n > b->bytes*8 - b->pos) {
        b->error = 1;
        return 0;
    }
    if (!n) return 0;
#if ET_CACHED_BITS && ET_FAST_HEADERS
    /* At most 24 bits plus a seven-bit offset: one bounded 32-bit
     * big-endian read suffices. No context construction for every header. */
    const uint8_t *source;
#if ET_DIRECT_BITS
    if (b->bytes - (b->pos >> 3) >= 4) source = b->data + (b->pos >> 3);
    else
#endif
    {
        bit_refill(b, 8);
        source = b->window + (b->pos >> 3) - b->window_base;
    }
    uint32_t word = AV_RB32(source);
    unsigned val = (word << (b->pos & 7)) >> (32 - n);
#else
    uint8_t buf[72];
    GetBitContext gb;
    bit_window(b, buf, 8, &gb);
    unsigned val = get_bits(&gb, n);
#endif
    b->pos += n;
    return val;
}

static int read_vlc(ETBits *b, const VLCElem *table)
{
    uint8_t buf[72];
    GetBitContext gb;
    if (b->error) return -1;
    bit_window(b, buf, 8, &gb);
    unsigned start = get_bits_count(&gb);
    int sym = get_vlc2(&gb, table, 9, 2);
    unsigned used = get_bits_count(&gb) - start;
    if (sym < 0 || !used || used > b->bytes*8 - b->pos) {
        b->error = 1;
        return -1;
    }
    b->pos += used;
    return sym;
}

static int get_qscale(ETBits *b, const ETFrameParams *p)
{
    unsigned q = read_bits(b, 5);
    return p->q_scale_type ? et_ff_mpeg2_non_linear_qscale[q] : 2*q;
}

/* MPEG address increments; escaping and stuffing are bounded by input.
 * A slice must begin with increment=1 even in P/B pictures. */
static int read_increment(ETBits *b, unsigned limit)
{
    unsigned inc = 0;
    for (;;) {
        int code = read_vlc(b, et_mbincr_vlc);
        if (code < 0 || code == 35) return -1;
        if (code == 34) continue;
        if (code == 33) inc += 33;
        else inc += code + 1;
        if (inc > limit) return -2;
        if (code < 33) return inc;
    }
}

static int block_decode(ETBits *b, ETBlockState *s, int16_t *block, int n, int intra)
{
    _Alignas(8) uint8_t scratch[320];
    s->valid_bits = bit_window(b, scratch, 256, &s->gb);
    unsigned start = get_bits_count(&s->gb);
    int result = intra ? mpeg2_decode_block_intra(s, block, n) :
                         mpeg2_decode_block_non_intra(s, block, n);
    unsigned used = get_bits_count(&s->gb) - start;
    if (result || used > b->bytes*8 - b->pos) return ET_DECODE_BAD_SLICE;
    b->pos += used;
    return ET_DECODE_OK;
}

enum { ET_MB_INTRA=1, ET_MB_PATTERN=2, ET_MB_BACKWARD=4,
       ET_MB_FORWARD=8, ET_MB_QUANT=16 };

typedef struct ETMotionState {
    int mv[2][2];
    unsigned previous_type;
} ETMotionState;

/* MPEG2 motion differential and modulo reconstruction, adapted directly from
 * FFmpeg mpeg12dec.c:mpeg_decode_motion. Valid f_codes are checked per frame. */
static int decode_motion(ETBits *b, unsigned fcode, int pred)
{
    int code = read_vlc(b, et_mv_vlc);
    if (code < 0) return 0;
    if (!code) return pred;
    int sign = read_bits(b, 1);
    unsigned shift = fcode - 1;
    int value = code;
    if (shift) value = ((value - 1) << shift) + read_bits(b, shift) + 1;
    if (sign) value = -value;
    value += pred;
    unsigned mask = (1u << (5 + shift)) - 1;
    unsigned wrapped = (unsigned)value & mask;
    return wrapped & ((mask + 1) >> 1) ? (int)wrapped - (int)(mask + 1) : (int)wrapped;
}

/* FFmpeg mpegvideo_motion.c's frame MPEG2 4:2:0 mapping: chroma motion is
 * divided by two toward zero, then split into integer / half-pel components.
 * MPEG2 motion outside the padded reference picture is malformed, unlike
 * codecs that permit edge emulation. Reject it instead of reading past DRAM.
 * Rounding follows generic put_pixels*_x2/y2/xy2 and avg_pixels in hpeldsp. */
static int predict_mb(const ETFrameParams *p, unsigned x, unsigned y,
                      unsigned type, const ETMotionState *m)
{
    int have_prediction = 0;
    for (unsigned direction = 0; direction < 2; direction++) {
        unsigned flag = direction ? ET_MB_BACKWARD : ET_MB_FORWARD;
        if (!(type & flag)) continue;
        uint64_t ref_addr = direction ? p->ref_bwd_addr : p->ref_fwd_addr;
        if (!ref_addr) return ET_DECODE_BAD_PARAMS;
        for (unsigned plane = 0; plane < 3; plane++) {
            int size = plane ? 8 : 16;
            int mx = plane ? m->mv[direction][0] / 2 : m->mv[direction][0];
            int my = plane ? m->mv[direction][1] / 2 : m->mv[direction][1];
            int sx = x * size + (mx >> 1), sy = y * size + (my >> 1);
            int hx = mx & 1, hy = my & 1;
            if (sx < 0 || sy < 0 || sx + size + hx > (int)p->mb_width*size ||
                sy + size + hy > (int)p->mb_height*size)
                return ET_DECODE_BAD_SLICE;
            unsigned stride = plane ? p->linesize_uv : p->linesize_y;
            const uint8_t *src = (const uint8_t *)(uintptr_t)ref_addr +
                p->plane_offset[plane] + (size_t)sy*stride + sx;
            uint8_t *dst = (uint8_t *)(uintptr_t)p->dst_addr +
                p->plane_offset[plane] + (size_t)y*size*stride + x*size;
#if ET_PREFETCH_MC
            et_prefetch_rectangle(src, stride, size + hx, size + hy);
#endif
            et_mc_predict(dst, src, stride, size, hx, hy, have_prediction);
        }
        have_prediction = 1;
    }
    return have_prediction ? ET_DECODE_OK : ET_DECODE_BAD_SLICE;
}

static void reset_dc(ETBlockState *s)
{
    s->last_dc[0] = s->last_dc[1] = s->last_dc[2] = 1 << (7 + s->intra_dc_precision);
}

static int decode_mb(const ETFrameParams *p, ETBits *b, ETBlockState *s,
                     ETMotionState *motion, unsigned x, unsigned y)
{
    int type;
    if (p->pict_type == 1) {
        int quant = !read_bits(b, 1);
        if (quant && !read_bits(b, 1)) return ET_DECODE_BAD_SLICE;
        type = ET_MB_INTRA | (quant ? ET_MB_QUANT : 0);
    } else {
        type = read_vlc(b, p->pict_type == 2 ? et_mbptype_vlc : et_mbbtype_vlc);
        if (type < 0) return ET_DECODE_BAD_SLICE;
    }
    int intra = type & ET_MB_INTRA;
    int field_dct = intra && !p->frame_pred_frame_dct ? read_bits(b, 1) : 0;
    if (type & ET_MB_QUANT) et_set_qscale(s, get_qscale(b, p));
    if (b->error || !s->qscale) return ET_DECODE_BAD_SLICE;
    int cbp = 63;
    if (intra) {
        memset(motion->mv, 0, sizeof(motion->mv));
    } else {
        reset_dc(s);
        /* P pattern without FOR means forward prediction with zero vector. */
        if (p->pict_type == 2 && !(type & ET_MB_FORWARD)) {
            type |= ET_MB_FORWARD;
            motion->mv[0][0] = motion->mv[0][1] = 0;
        } else {
            for (unsigned direction = 0; direction < 2; direction++) {
                if (type & (direction ? ET_MB_BACKWARD : ET_MB_FORWARD)) {
                    for (unsigned axis = 0; axis < 2; axis++)
                        motion->mv[direction][axis] = decode_motion(b, p->f_code[direction][axis],
                                                                  motion->mv[direction][axis]);
                }
            }
        }
        if (b->error) return ET_DECODE_BAD_SLICE;
        int ret = predict_mb(p, x, y, type, motion);
        if (ret) return ret;
        cbp = type & ET_MB_PATTERN ? read_vlc(b, et_mbpat_vlc) : 0;
        if (b->error || cbp < 0 || (!cbp && (type & ET_MB_PATTERN))) return ET_DECODE_BAD_SLICE;
    }
    for (int n = 0; n < 6; n++) {
        if (!(cbp & (32 >> n))) continue;
        _Alignas(32) int16_t block[64] = {0};
        int ret = block_decode(b, s, block, n, intra);
        if (ret) return ret;
        unsigned plane = n < 4 ? 0 : n-3;
        unsigned stride = plane ? p->linesize_uv : p->linesize_y;
        unsigned px = plane ? 8*x : 16*x + (n&1)*8;
        unsigned py = plane ? 8*y : 16*y + (n>>1)*(field_dct ? 1 : 8);
        uint8_t *dst = (uint8_t *)(uintptr_t)p->dst_addr + p->plane_offset[plane] + (size_t)py*stride + px;
        unsigned output_stride = stride * (!plane && field_dct ? 2 : 1);
#if ET_FAST_DC
        if (et_reconstruct_dc(dst, output_stride, block, !intra)) continue;
#endif
        if (intra) ff_simple_idct_put_int16_8bit(dst, output_stride, block);
        else ff_simple_idct_add_int16_8bit(dst, stride, block);
    }
    motion->previous_type = type;
    return ET_DECODE_OK;
}

static void evict_row(const ETFrameParams *p, unsigned y)
{
    uint8_t *dst = (uint8_t *)(uintptr_t)p->dst_addr;
    for (unsigned plane = 0; plane < 3; plane++) {
        unsigned rows = plane ? 8 : 16;
        unsigned stride = plane ? p->linesize_uv : p->linesize_y;
        et_evict(dst + p->plane_offset[plane] + (size_t)y*rows*stride,
                 (size_t)rows*stride);
    }
}

static int decode_slice(const ETFrameParams *p, const ETSliceDesc *slice,
                        ETSliceStatus *status)
{
    const uint8_t *input = (const uint8_t *)(uintptr_t)p->input_addr;
    uint64_t end = (uint64_t)p->bitstream_offset + slice->bitstream_off + slice->bitstream_len;
    if (end > p->input_bytes || slice->bitstream_len < 5 ||
        slice->bitstream_len > UINT32_MAX/8 || slice->reserved)
        return ET_DECODE_BAD_SLICE;
    ETBits b;
    b.data = input + p->bitstream_offset + slice->bitstream_off;
    b.bytes = slice->bitstream_len;
    b.pos = 32;
    b.error = 0;
#if ET_CACHED_BITS
    b.window_base = b.window_bytes = 0;
#endif
    if (b.data[0] || b.data[1] || b.data[2] != 1 ||
        b.data[3] != slice->mb_y + 1)
        return ET_DECODE_BAD_SLICE;
    /* Every live field is initialized below or by block_decode; do not
     * clear a matrix cache that the first qscale setup overwrites fully. */
    ETBlockState s;
    s.qscale = 0;
    const uint16_t *mat = (const uint16_t *)(input + p->quant_offset);
#if ET_PREQUANT
    s.raw_quant = mat;
    mat = s.scaled_quant;
#endif
    s.intra_matrix = mat;
    s.chroma_intra_matrix = mat + 128;
    s.inter_matrix = mat + 64;
    s.chroma_inter_matrix = mat + 192;
    s.intra_scantable.permutated = p->alternate_scan ?
        et_ff_alternate_vertical_scan : et_ff_zigzag_direct;
    s.intra_dc_precision = p->intra_dc_precision;
    s.intra_vlc_format = p->intra_vlc_format;
    s.last_dc[0] = s.last_dc[1] = s.last_dc[2] = 1 << (7 + p->intra_dc_precision);
    et_set_qscale(&s, get_qscale(&b, p));
    if (!s.qscale || (slice->quant_scale && s.qscale != slice->quant_scale))
        return ET_DECODE_BAD_SLICE;
    while (read_bits(&b, 1)) read_bits(&b, 8);
    if (b.error) return ET_DECODE_BAD_SLICE;

    ETMotionState motion = {0};
    for (unsigned x = 0; x < p->mb_width;) {
        int inc = read_increment(&b, p->mb_width-x);
        if (inc == -2) return ET_DECODE_ROW_OVERFLOW;
        if (inc < 1 || (!x && inc != 1) || (p->pict_type == 1 && inc != 1))
            return ET_DECODE_BAD_SLICE;
        while (inc-- > 1) {
            reset_dc(&s);
            if (p->pict_type == 2) {
                motion.mv[0][0] = motion.mv[0][1] = 0;
                motion.previous_type = ET_MB_FORWARD;
            } else if (motion.previous_type & ET_MB_INTRA) {
                return ET_DECODE_BAD_SLICE;
            }
            int ret = predict_mb(p, x, slice->mb_y, motion.previous_type, &motion);
            if (ret) return ret;
            x++;
            status->mb_decoded++;
            status->bits_consumed = b.pos;
        }
        int ret = decode_mb(p, &b, &s, &motion, x, slice->mb_y);
        if (ret) return ret;
        x++;
        status->mb_decoded++;
        status->bits_consumed = b.pos;
    }
    /* Only alignment zero bits / zero byte stuffing may remain. A second
     * row (even a valid coded one), partial row, or next start code is rejected. */
    while (b.pos < b.bytes*8) {
        unsigned n = b.bytes*8 - b.pos;
        if (n > 24) n = 24;
        if (read_bits(&b, n)) return ET_DECODE_ROW_OVERFLOW;
    }
    return b.error ? ET_DECODE_BAD_SLICE : ET_DECODE_OK;
}

static int params_valid(const ETFrameParams *p)
{
    if (p->abi_version != ET_MPEG2_ABI_VERSION || p->reserved16 || !p->frame_id ||
        (p->active_harts != 1 && p->active_harts != 64) ||
        !p->nb_slices || p->nb_slices > ET_MPEG2_MAX_SLICES ||
        !p->mb_width || p->mb_width > 1024 || !p->mb_height || p->mb_height > 175 ||
        p->nb_slices != p->mb_height || !p->width || !p->height ||
        p->mb_width != ((uint64_t)p->width + 15)/16 ||
        p->mb_height != ((uint64_t)p->height + 15)/16 ||
        !p->input_addr || !p->dst_addr || !p->status_addr ||
        (p->input_addr & 7) || (p->dst_addr & 63) || (p->status_addr & 63) ||
        (p->slice_table_offset & 3) || (p->quant_offset & 1) ||
        (p->linesize_y & 63) || (p->linesize_uv & 63) ||
        p->linesize_y < 16*p->mb_width || p->linesize_uv < 8*p->mb_width ||
        p->input_bytes > UINT32_MAX/8 ||
        (uint64_t)p->slice_table_offset + p->nb_slices*sizeof(ETSliceDesc) > p->input_bytes ||
        (uint64_t)p->quant_offset + 4*64*sizeof(uint16_t) > p->input_bytes ||
        p->bitstream_offset > p->input_bytes ||
        p->input_addr > UINTPTR_MAX-p->input_bytes ||
        p->dst_addr > UINTPTR_MAX-p->frame_bytes ||
        p->status_addr > UINTPTR_MAX-p->nb_slices*sizeof(ETSliceStatus))
        return ET_DECODE_BAD_PARAMS;
    /* These allocations must not alias: a status or reconstructed row must
     * never overwrite descriptors / matrices read later by another hart. */
    uint64_t input_end = p->input_addr + p->input_bytes;
    uint64_t output_end = p->dst_addr + p->frame_bytes;
    uint64_t status_end = p->status_addr + p->nb_slices*sizeof(ETSliceStatus);
    if ((p->input_addr < output_end && p->dst_addr < input_end) ||
        (p->input_addr < status_end && p->status_addr < input_end) ||
        (p->dst_addr < status_end && p->status_addr < output_end))
        return ET_DECODE_BAD_PARAMS;
    if (p->pict_type < 1 || p->pict_type > 3 || p->picture_structure != 3 || p->progressive_frame > 1 ||
        p->frame_pred_frame_dct > 1 || p->concealment_mv ||
        p->intra_dc_precision > 3 || p->intra_vlc_format > 1 ||
        p->q_scale_type > 1 || p->alternate_scan > 1)
        return ET_DECODE_UNSUPPORTED;
    if (p->pict_type != 1) {
        /* Initial inter-picture surface: progressive frame motion only. */
        if (!p->progressive_frame || !p->frame_pred_frame_dct)
            return ET_DECODE_UNSUPPORTED;
        for (unsigned direction = 0; direction < (p->pict_type == 3 ? 2u : 1u); direction++) {
            uint64_t ref = direction ? p->ref_bwd_addr : p->ref_fwd_addr;
            if (!ref || (ref & 63) || ref > UINTPTR_MAX-p->frame_bytes)
                return ET_DECODE_BAD_PARAMS;
            uint64_t ref_end = ref+p->frame_bytes;
            if ((ref < output_end && p->dst_addr < ref_end) ||
                (ref < input_end && p->input_addr < ref_end) ||
                (ref < status_end && p->status_addr < ref_end))
                return ET_DECODE_BAD_PARAMS;
            for (unsigned axis = 0; axis < 2; axis++)
                if (!p->f_code[direction][axis] || p->f_code[direction][axis] > 9)
                    return ET_DECODE_UNSUPPORTED;
        }
    }
    uint64_t end[3];
    for (unsigned i = 0; i < 3; i++) {
        uint64_t stride = i ? p->linesize_uv : p->linesize_y;
        end[i] = p->plane_offset[i] + stride*p->mb_height*(i ? 8 : 16);
        if ((p->plane_offset[i] & 63) || end[i] > p->frame_bytes)
            return ET_DECODE_BAD_PARAMS;
        for (unsigned j = 0; j < i; j++)
            if (p->plane_offset[i] < end[j] && p->plane_offset[j] < end[i])
                return ET_DECODE_BAD_PARAMS;
    }
    const uint8_t *input = (const uint8_t *)(uintptr_t)p->input_addr;
    const uint16_t *mat = (const uint16_t *)(input + p->quant_offset);
    for (unsigned i = 0; i < 256; i++)
        if (!mat[i] || mat[i] > 255) return ET_DECODE_BAD_PARAMS;
    const ETSliceDesc *slices = (const ETSliceDesc *)(input + p->slice_table_offset);
    uint8_t seen[ET_MPEG2_MAX_SLICES] = {0};
    for (unsigned i = 0; i < p->nb_slices; i++) {
        if (slices[i].mb_y >= p->mb_height || seen[slices[i].mb_y])
            return ET_DECODE_BAD_PARAMS;
        seen[slices[i].mb_y] = 1;
    }
    return ET_DECODE_OK;
}

static int status_valid(const ETFrameParams *p)
{
    /* Do not dereference status if the status-array envelope is invalid. */
    if (!p || !p->nb_slices || !p->status_addr || (p->status_addr & 63) ||
        p->nb_slices > ET_MPEG2_MAX_SLICES ||
        p->status_addr > UINTPTR_MAX-p->nb_slices*sizeof(ETSliceStatus))
        return 0;
    uint64_t status_end = p->status_addr + p->nb_slices*sizeof(ETSliceStatus);
    if ((p->input_addr <= UINTPTR_MAX-p->input_bytes &&
         p->input_addr < status_end && p->status_addr < p->input_addr+p->input_bytes) ||
        (p->dst_addr <= UINTPTR_MAX-p->frame_bytes &&
         p->dst_addr < status_end && p->status_addr < p->dst_addr+p->frame_bytes))
        return 0;
    for (unsigned direction = 0; direction < 2; direction++) {
        uint64_t ref = direction ? p->ref_bwd_addr : p->ref_fwd_addr;
        if (ref && ref <= UINTPTR_MAX-p->frame_bytes && ref < status_end &&
            p->status_addr < ref+p->frame_bytes) return 0;
    }
    return 1;
}

int et_mpeg2_report_failure(const ETFrameParams *p, unsigned hart, int code)
{
    if (hart || !status_valid(p)) return code;
    ETSliceStatus *status = (ETSliceStatus *)(uintptr_t)p->status_addr;
    for (unsigned i = 0; i < p->nb_slices; i++) {
        ETSliceStatus st = {0};
        st.code = code;
        st.frame_id = p->frame_id;
        status[i] = st;
        et_evict(status+i, sizeof(*status));
    }
    return code;
}

int et_mpeg2_decode(const ETFrameParams *p, unsigned hart)
{
    if (!p) return ET_DECODE_BAD_PARAMS;
    if (p->active_harts != 1 && p->active_harts != 64)
        return et_mpeg2_report_failure(p, hart, ET_DECODE_BAD_PARAMS);
    if (hart >= p->active_harts) return ET_DECODE_OK;
#if ET_EVEN_ONLY
    if (p->active_harts == 64 && (hart & 1)) return ET_DECODE_OK;
#endif
    int ret = params_valid(p);
    if (!status_valid(p)) return ET_DECODE_BAD_PARAMS;
    ETSliceStatus *status = (ETSliceStatus *)(uintptr_t)p->status_addr;
    const ETSliceDesc *slices = ret ? NULL :
        (const ETSliceDesc *)((const uint8_t *)(uintptr_t)p->input_addr + p->slice_table_offset);
    int first_error = ret;
    /* Use one hart from EVERY minion before assigning its SMT sibling.
     * With 36 SD rows, linear IDs occupy only 18/32 minions. The permutation
     * preserves complete row/cache-line ownership and the 1/64-hart ABI. */
    unsigned worker = hart, workers = p->active_harts;
#if ET_EVEN_ONLY
    if (p->active_harts == 64) { worker = hart >> 1; workers = 32; }
#elif ET_SPREAD_HARTS
    if (p->active_harts == 64) worker = (hart >> 1) + 32 * (hart & 1);
#endif
    for (unsigned i = worker; i < p->nb_slices; i += workers) {
        uint64_t start = et_cycles();
        ETSliceStatus st = {0};
        st.frame_id = p->frame_id;
        st.code = ret ? ret : decode_slice(p, slices+i, &st);
        if (!ret) evict_row(p, slices[i].mb_y);
        if (st.code && !first_error) first_error = st.code;
        st.cycles = et_cycles()-start;
        status[i] = st;
        et_evict(status+i, sizeof(*status));
    }
    return first_error;
}
