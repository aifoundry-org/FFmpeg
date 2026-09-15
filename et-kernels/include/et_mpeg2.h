/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef ET_KERNEL_MPEG2_H
#define ET_KERNEL_MPEG2_H
#include "libavcodec/et_mpeg2_protocol.h"
/* hart is a zero-based logical hart within the single selected shire.
 * Addresses are native pointers for the native build, device VAs on ET.
 * The return is the first error on this hart (also stored per-slice).
 * Harts >= active_harts do nothing. No allocation or mutable globals. */
int et_mpeg2_decode(const ETFrameParams *params, unsigned hart);
#endif
