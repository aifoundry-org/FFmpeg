# AAC DSP prototype provenance

This directory is an isolated, freestanding extraction for the ETSOC AAC-LC
prototype.  It is derived from this repository's pinned FFmpeg n7.1.1 source,
commit `db69d06eeeab4f46da15030a80d539efb4503ca8` (checkout currently carries
later documentation commits).  The derived implementation is LGPL-2.1-or-later.

## Attribution and pinned inputs

The copied/derived transform material retains FFmpeg's LGPL-2.1-or-later
license and its source attribution: `tx_template.c` credits Lynne, Loren
Merritt (2008), Fabrice Bellard (2002), and libdjbfft by D. J. Bernstein;
`aacdec_dsp_template.c` credits Oded Shimon, Maxim Gavrilov, Alex Converse and
others named in its source header; `float_dsp.c` credits Balatoni Denes and
Loren Merritt. The window/table inputs retain the attribution in their pinned
FFmpeg source headers.

* `libavutil/tx_template.c`: float split-radix inverse FFT, `ff_tx_mdct_inv`,
  transform exponent generation and power-of-two tables.
* `libavutil/tx_priv.h`: `CMUL`, `BF`, and split-radix operation semantics.
* `libavutil/tx.c`: split-radix permutation generator.
* `libavcodec/aac/aacdec_dsp_template.c`: AAC `imdct_and_windowing` and its
  scalar `vector_fmul_window` call order.
* `libavutil/float_dsp.c`: scalar window-multiply implementation.
* `libavcodec/{aactab.c,kbdwin.c,sinewin_tablegen.h}` and
  `libavutil/mathematics.c`: window initialization used only by the host table
  generator.

`tools/generate_tables.c` is the scripted provenance step. It reproduces the
pinned initialization arithmetic and emits immutable IEEE-754 bit-pattern
arrays in `etaac_tables.h`. Production `etaac_synth.c` uses no libm, heap,
initialization, callbacks, or mutable static storage. Regenerate with
`tools/generate_tables.sh`; review the generated diff.

The arithmetic contract is the pinned scalar float codelet selected with CPU
flags forced to zero: 128 and 1024 coefficient MDCTs, with AAC scale
`(1.0f/n)/32768.0f`. Build production code with FP contraction disabled
(e.g. `-ffp-contract=off`); no FMA contraction is permitted.
