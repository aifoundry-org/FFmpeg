/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "protocol.h"
#include "../dsp/synth.h"
#include "../../et-kernels/src/cache.h"
#include <string.h>

#ifndef ETAAC_FAST_FINITE
#define ETAAC_FAST_FINITE 0
#endif
#if ETAAC_FAST_FINITE
/* All protocol buffers and DSP scratch are at least 4-byte aligned. may_alias
 * permits inspecting IEEE bits without changing float arithmetic or checks. */
typedef uint32_t alias_u32 __attribute__((may_alias));
#endif
static int finite_samples(const float *x, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        uint32_t bits;
        #if ETAAC_FAST_FINITE
        bits = ((const alias_u32 *)x)[i];
#else
        memcpy(&bits, x + i, sizeof(bits));
#endif
        if ((bits & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000))
            return 0;
    }
    return 1;
}

static int input_metadata_valid(const ETAACInput *in, uint64_t expected)
{
    unsigned i;
    if (!in->stream_id || in->sequence > 3 || in->shape > 1 ||
        in->expected_generation != expected)
        return ETAAC_BAD_INPUT;
    for (i = 0; i < 5; i++)
        if (in->reserved[i])
            return ETAAC_BAD_INPUT;
    return ETAAC_OK;
}

/* This is deliberately checked before copying the overlap into the worker's
 * hot scratch area.  Thus a poisoned or malformed slot never causes a state
 * copy, and a structurally invalid launch never reaches any buffer address. */
static int state_valid(const ETAACState *s, const ETAACInput *in, uint64_t expected)
{
    unsigned i;
    int rc = ETAAC_OK;
    if (s->initialized > 1 || s->reserved0 || s->sequence > 3 || s->shape > 1 ||
        s->generation != expected || (s->initialized && s->stream_id != in->stream_id))
        rc = ETAAC_BAD_STATE;
    for (i = 0; i < 4; i++)
        if (s->reserved[i])
            rc = ETAAC_BAD_STATE;
    if (!s->initialized && (s->generation || s->stream_id || s->sequence || s->shape))
        rc = ETAAC_BAD_STATE;
    if (!s->initialized) {
        for (i = 0; i < ETAAC_SAVED; i++)
            if (s->saved[i] != 0.0f)
                rc = ETAAC_BAD_STATE;
    }
    /* Keep v1's classification for non-finite state data. */
    if (!finite_samples(s->saved, ETAAC_SAVED))
        rc = ETAAC_BAD_INPUT;
    return rc;
}

int etaac_offline_process(const ETAACOfflineParams *p, unsigned hart)
{
    ETAACInput *inputs;
    ETAACState *states;
    ETAACStatus *statuses;
    float *outputs;
    float *scratch;
    float *saved;
    unsigned worker, i;
    int result = ETAAC_OK;
    uint64_t end_generation;

    /* Do not dereference a params pointer for a non-existent hart. */
    if (hart >= ETAAC_HARTS)
        return ETAAC_BAD_PARAMS;
    if (!etaac_offline_valid(p))
        return ETAAC_BAD_PARAMS;
    end_generation = p->generation + p->frames - 1;

    worker = (hart >> 1) + 32 * (hart & 1);
    if (worker >= p->active_harts)
        return ETAAC_OK;

    inputs = (ETAACInput *)(uintptr_t)p->input_addr;
    states = (ETAACState *)(uintptr_t)p->state_addr;
    outputs = (float *)(uintptr_t)p->output_addr;
    scratch = (float *)(uintptr_t)p->scratch_addr + worker * ETAAC_SCRATCH_FLOATS;
    statuses = (ETAACStatus *)(uintptr_t)p->status_addr;
    saved = scratch + 4096;

    for (i = worker; i < p->count; i += p->active_harts) {
        ETAACState *s = &states[i];
        ETAACStatus *st = &statuses[i];
        uint64_t stream_id = s->stream_id;
        uint64_t generation = s->generation;
        uint64_t status_stream = 0;
        unsigned sequence = s->sequence;
        unsigned shape = s->shape;
        unsigned initialized = s->initialized;
        unsigned successful = 0;
        unsigned attempted = 0;
        unsigned f;
        int rc;
        const unsigned first_index = p->frame_offset * p->count + i;
        const ETAACInput *first = &inputs[first_index];

        status_stream = first->stream_id;
        rc = input_metadata_valid(first, p->generation - 1);
        {
            int src = state_valid(s, first, p->generation - 1);
            if (src)
                rc = src;
        }
        if (!finite_samples(first->coeff, ETAAC_SAMPLES))
            rc = ETAAC_BAD_INPUT;
        if (!rc)
            memcpy(saved, s->saved, sizeof(s->saved));

        for (f = 0; !rc && f < p->frames; f++) {
            const unsigned index = (p->frame_offset + f) * p->count + i;
            const ETAACInput *in = &inputs[index];
            float *out = outputs + (size_t)index * ETAAC_SAMPLES;
            const uint64_t expected = p->generation + f - 1;

            status_stream = in->stream_id;
            rc = input_metadata_valid(in, expected);
            if (generation != expected || (initialized && stream_id != in->stream_id))
                rc = ETAAC_BAD_STATE;
            if (!finite_samples(in->coeff, ETAAC_SAMPLES))
                rc = ETAAC_BAD_INPUT;
            if (rc)
                break;

            attempted = 1;
            if (p->operation == ETAAC_COPY)
                memcpy(out, in->coeff, ETAAC_SAMPLES * sizeof(*out));
            else if (etaac_synth(out, saved, in->coeff, in->sequence, sequence,
                                 in->shape, shape, scratch))
                rc = ETAAC_BAD_INPUT;
            if (!finite_samples(out, ETAAC_SAMPLES) ||
                !finite_samples(saved, ETAAC_SAVED))
                rc = ETAAC_BAD_INPUT;
            if (rc)
                break;

            stream_id = in->stream_id;
            generation = p->generation + f;
            sequence = in->sequence;
            shape = in->shape;
            initialized = 1;
            successful++;
            /* Every completed PCM block is consumable by a later launch. */
            et_evict(out, ETAAC_SAMPLES * sizeof(*out));
        }

        if (!rc || successful || attempted) {
            memcpy(s->saved, saved, sizeof(s->saved));
            s->stream_id = stream_id;
            s->generation = generation;
            s->sequence = sequence;
            s->shape = shape;
            /* A failed calculation, or a later malformed frame after a
             * successful prefix, cannot be resumed by another launch. */
            s->initialized = rc ? 2 : initialized;
            et_evict(s, sizeof(*s));
        }

        memset(st, 0, sizeof(*st));
        st->generation = end_generation;
        st->stream_id = status_stream;
        st->result = rc;
        st->samples = successful * ETAAC_SAMPLES;
        et_evict(st, sizeof(*st));
        if (rc && !result)
            result = rc;
    }
    return result;
}

#ifdef ET_DEVICE
typedef struct ETKernelEnvironment {
    uint16_t major, minor, patch, reserved;
    uint64_t shire_mask;
    uint32_t frequency, padding;
} ETKernelEnvironment;

int entry_point(const ETAACOfflineParams *p, const void *environment)
{
    const ETKernelEnvironment *env = environment;
    unsigned long hart;
    __asm__ volatile("csrr %0, hartid" : "=r"(hart));
    if (!env || env->shire_mask != 1 || hart >= ETAAC_HARTS)
        return ETAAC_BAD_PARAMS;
    return etaac_offline_process(p, (unsigned)hart);
}
#endif
