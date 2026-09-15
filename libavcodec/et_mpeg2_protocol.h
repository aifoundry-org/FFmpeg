/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef AVCODEC_ET_MPEG2_PROTOCOL_H
#define AVCODEC_ET_MPEG2_PROTOCOL_H

#include <stdint.h>

#define ET_MPEG2_ABI_VERSION 1
#define ET_CACHE_LINE 64
#define ET_MPEG2_HARTS 64
#define ET_MPEG2_MAX_SLICES 256

/* All addresses refer to device DRAM. Offsets are relative to input_addr,
 * except ETSliceDesc.bitstream_off, which is relative to bitstream_offset.
 * Quant matrices: intra, inter, chroma intra, chroma inter; uint16_t[4][64]
 * in natural coefficient order. The scalar IDCT uses identity permutation. */
typedef struct ETFrameParams {
    uint64_t input_addr;
    uint64_t dst_addr;
    uint64_t ref_fwd_addr;
    uint64_t ref_bwd_addr;
    uint64_t status_addr;
    uint32_t abi_version;
    uint32_t nb_slices;
    uint32_t mb_width;
    uint32_t mb_height;
    uint32_t width;
    uint32_t height;
    uint32_t linesize_y;
    uint32_t linesize_uv;
    uint32_t slice_table_offset;
    uint32_t bitstream_offset;
    uint32_t quant_offset;
    uint32_t frame_bytes;
    uint32_t plane_offset[3];
    uint16_t pict_type;
    uint16_t picture_structure;
    uint8_t q_scale_type;
    uint8_t intra_dc_precision;
    uint8_t intra_vlc_format;
    uint8_t alternate_scan;
    uint8_t concealment_mv;
    uint8_t frame_pred_frame_dct;
    uint8_t top_field_first;
    uint8_t progressive_frame;
    uint8_t f_code[2][2];
    uint16_t active_harts;
    uint16_t reserved16;
    uint32_t input_bytes;
    uint32_t frame_id; /* Nonzero generation echoed in each slice status. */
} ETFrameParams;

typedef struct ETSliceDesc {
    uint32_t bitstream_off;
    uint32_t bitstream_len; /* Includes the four-byte slice start code. */
    uint16_t mb_y;
    uint16_t quant_scale;
    uint32_t reserved;
} ETSliceDesc;

enum ETDecodeStatus {
    ET_DECODE_OK = 0,
    ET_DECODE_PENDING = 1,
    ET_DECODE_BAD_PARAMS = 2,
    ET_DECODE_BAD_SLICE = 3,
    ET_DECODE_UNSUPPORTED = 4,
    ET_DECODE_ROW_OVERFLOW = 5,
};

/* One cache line per slice: no two harts write the same status line. */
typedef struct ETSliceStatus {
    uint32_t code;
    uint32_t mb_decoded;
    uint32_t bits_consumed;
    uint32_t frame_id;
    uint64_t cycles;
    uint8_t padding[40];
} ETSliceStatus;

#if defined(__cplusplus)
static_assert(sizeof(ETFrameParams) == 128, "ET inline launch ABI");
static_assert(sizeof(ETSliceDesc) == 16, "ET slice ABI");
static_assert(sizeof(ETSliceStatus) == 64, "ET status ABI");
#else
_Static_assert(sizeof(ETFrameParams) == 128, "ET inline launch ABI");
_Static_assert(sizeof(ETSliceDesc) == 16, "ET slice ABI");
_Static_assert(sizeof(ETSliceStatus) == 64, "ET status ABI");
#endif

#endif
