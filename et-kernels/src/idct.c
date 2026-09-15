/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Compile FFmpeg's scalar template unchanged, identity coefficient ordering. */
#include "config.h"
#include "libavutil/intreadwrite.h"
#include "libavcodec/mathops.h"
#define IN_IDCT_DEPTH 16
#define BIT_DEPTH 8
#include "libavcodec/simple_idct_template.c"
