/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef ET_DECODER_INTERNAL_H
#define ET_DECODER_INTERNAL_H
#include "et_mpeg2.h"
/* Fail a launch without decoding. Logical hart zero publishes every safe
 * status record, including the generation token. Others only return code. */
int et_mpeg2_report_failure(const ETFrameParams *p, unsigned hart, int code);
#endif
