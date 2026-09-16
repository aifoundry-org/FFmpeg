#!/bin/sh
# Regenerate immutable tables from the pinned FFmpeg scalar initialization.
set -eu
cd "$(dirname "$0")/.."
: "${CC:=cc}"
"$CC" -std=c11 -O2 -Wall -Wextra -Werror tools/generate_tables.c -lm -o /tmp/etaac-generate-tables
/tmp/etaac-generate-tables > etaac_tables.h
rm -f /tmp/etaac-generate-tables
sha256sum \
  ../../libavutil/tx_template.c ../../libavutil/tx_priv.h ../../libavutil/tx.c \
  ../../libavcodec/aac/aacdec_dsp_template.c ../../libavutil/float_dsp.c \
  ../../libavcodec/aactab.c ../../libavcodec/kbdwin.c \
  ../../libavcodec/sinewin_tablegen.h ../../libavutil/mathematics.c \
  > SOURCE_MANIFEST.sha256
