/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef AVCODEC_ET_MPEG12_H
#define AVCODEC_ET_MPEG12_H
#include "avcodec.h"
int ff_et_mpeg2_enabled(const AVCodecContext *avctx);
int ff_et_mpeg2_probe(AVCodecContext *avctx);
struct MPVPicture;
int ff_et_mpeg2_picture_valid(AVCodecContext *avctx, const struct MPVPicture *pic);
#endif
