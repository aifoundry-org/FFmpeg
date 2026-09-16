/*
 * AAC-LC float IMDCT/window fixture capture format, version 1.
 *
 * All integers and float bit patterns are written explicitly little-endian.
 * A file begins with this 32-byte header:
 *   byte[8] magic       "ETAACCP1" (no NUL)
 *   u32     version     1
 *   u32     header_size 32
 *   u32     record_size 12312
 *   u32     float_format 1 (IEEE-754 binary32 bits)
 *   u32     endian_tag  0x01020304
 *   u32     flags       0
 *
 * It is followed by zero or more 12,312-byte records. Each record is:
 *   u32 context_id                 // process-local saved-state identity, starts at 1
 *   u32 frame_number               // monotonically increasing within context, starts at 0
 *   u32 window_sequence            // current IndividualChannelStream.window_sequence[0]
 *   u32 previous_window_sequence   // IndividualChannelStream.window_sequence[1]
 *                                      (AAC enum: 0 ONLY_LONG, 1 LONG_START,
 *                                       2 EIGHT_SHORT, 3 LONG_STOP)
 *   u32 window_shape               // current use_kb_window[0], 0 sine / 1 KBD
 *   u32 previous_window_shape      // use_kb_window[1], 0 sine / 1 KBD
 *   f32 coeff[1024]                // immediately before IMDCT
 *   f32 prior_saved[512]           // overlap state immediately before IMDCT
 *   f32 expected_out[1024]         // decoder output after window/overlap
 *   f32 next_saved[512]            // overlap state after update
 *
 * There is deliberately no padding, native struct serialization, channel-layout
 * field, or packet offset. context_id identifies the state vector consumed by
 * this record; thus every record can be replayed from its own prior_saved.
 */
#ifndef ETAAC_CAPTURE_FORMAT_H
#define ETAAC_CAPTURE_FORMAT_H
#include <stdint.h>
#define ETAAC_CAPTURE_MAGIC "ETAACCP1"
#define ETAAC_CAPTURE_VERSION 1u
#define ETAAC_CAPTURE_HEADER_BYTES 32u
#define ETAAC_CAPTURE_RECORD_BYTES 12312u
#define ETAAC_CAPTURE_FLOAT_IEEE754_BINARY32 1u
#define ETAAC_CAPTURE_ENDIAN_TAG 0x01020304u
#endif
