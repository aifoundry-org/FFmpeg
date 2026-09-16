/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "protocol.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *z(size_t n)
{
    void *p = NULL;
    assert(!posix_memalign(&p, 64, n + 64));
    memset(p, 0, n);
    memset((char *)p + n, 0xa5, 64);
    return p;
}

static void check_guard(const void *p, size_t n)
{
    unsigned i;
    for (i = 0; i < 64; i++)
        assert(((const unsigned char *)p)[n + i] == 0xa5);
}

typedef struct Buffers {
    ETAACInput *in;
    ETAACState *state;
    float *out;
    float *scratch;
    ETAACStatus *status;
    size_t input_bytes, state_bytes, output_bytes, scratch_bytes, status_bytes;
} Buffers;

static Buffers buffers(unsigned capacity, unsigned count, unsigned harts)
{
    Buffers b;
    b.input_bytes = (size_t)capacity * count * sizeof(*b.in);
    b.state_bytes = (size_t)count * sizeof(*b.state);
    b.output_bytes = (size_t)capacity * count * ETAAC_SAMPLES * sizeof(*b.out);
    b.scratch_bytes = (size_t)harts * ETAAC_SCRATCH_FLOATS * sizeof(*b.scratch);
    b.status_bytes = (size_t)count * sizeof(*b.status);
    b.in = z(b.input_bytes);
    b.state = z(b.state_bytes);
    b.out = z(b.output_bytes);
    b.scratch = z(b.scratch_bytes);
    b.status = z(b.status_bytes);
    return b;
}

static void free_buffers(Buffers *b)
{
    free(b->in);
    free(b->state);
    free(b->out);
    free(b->scratch);
    free(b->status);
}

static ETAACOfflineParams params(const Buffers *b, unsigned capacity, unsigned count,
                                 unsigned harts)
{
    ETAACOfflineParams p;
    memset(&p, 0, sizeof(p));
    p.abi_version = ETAAC_OFFLINE_ABI;
    p.count = count;
    p.active_harts = harts;
    p.input_addr = (uintptr_t)b->in;
    p.input_bytes = b->input_bytes;
    p.state_addr = (uintptr_t)b->state;
    p.state_bytes = b->state_bytes;
    p.output_addr = (uintptr_t)b->out;
    p.output_bytes = b->output_bytes;
    p.scratch_addr = (uintptr_t)b->scratch;
    p.scratch_bytes = b->scratch_bytes;
    p.status_addr = (uintptr_t)b->status;
    p.status_bytes = b->status_bytes;
    p.capacity_frames = capacity;
    p.frames = 1;
    p.generation = 1;
    return p;
}

static int run_harts(const ETAACOfflineParams *p)
{
    unsigned h;
    int result = ETAAC_OK;
    for (h = 0; h < ETAAC_HARTS; h++) {
        int rc = etaac_offline_process(p, h);
        if (rc && !result)
            result = rc;
    }
    return result;
}

static void fill_input(ETAACInput *in, unsigned channel, unsigned frame)
{
    unsigned j;
    memset(in, 0, sizeof(*in));
    in->stream_id = UINT64_C(1000) + channel;
    in->expected_generation = frame;
    in->sequence = frame & 3;
    in->shape = (channel + frame) & 1;
    for (j = 0; j < ETAAC_SAMPLES; j++)
        in->coeff[j] = (float)((int)((j * 719u + channel * 23u + frame * 373u) & 65535u) -
                               32768);
}

/* A frame-major 64-channel timeline must be bit-identical whether it is sent
 * one frame at a time, in eights, or in one 32-frame offline launch. */
static void split_launch_equivalence(void)
{
    enum { COUNT = 64, FRAMES = 32, OFFSET = 3, CAPACITY = OFFSET + FRAMES };
    const unsigned chunks[] = { 1, 8, 32 };
    const unsigned harts[] = { 64, 32, 1 };
    float *reference_out = NULL;
    ETAACState *reference_state = NULL;
    unsigned plan;

    for (plan = 0; plan < 3; plan++) {
        Buffers b = buffers(CAPACITY, COUNT, harts[plan]);
        ETAACOfflineParams p = params(&b, CAPACITY, COUNT, harts[plan]);
        unsigned f, i, done;
        for (f = 0; f < FRAMES; f++)
            for (i = 0; i < COUNT; i++)
                fill_input(&b.in[(OFFSET + f) * COUNT + i], i, f);
        assert(etaac_offline_valid(&p));

        for (done = 0; done < FRAMES; done += chunks[plan]) {
            p.frame_offset = OFFSET + done;
            p.frames = chunks[plan];
            p.generation = UINT64_C(1) + done;
            assert(!run_harts(&p));
            for (i = 0; i < COUNT; i++) {
                assert(b.status[i].generation == done + chunks[plan]);
                assert(b.status[i].result == ETAAC_OK);
                assert(b.status[i].samples == chunks[plan] * ETAAC_SAMPLES);
            }
        }

        if (!reference_out) {
            reference_out = malloc(b.output_bytes);
            reference_state = malloc(b.state_bytes);
            assert(reference_out && reference_state);
            memcpy(reference_out, b.out, b.output_bytes);
            memcpy(reference_state, b.state, b.state_bytes);
        } else {
            assert(!memcmp(reference_out, b.out, b.output_bytes));
            assert(!memcmp(reference_state, b.state, b.state_bytes));
        }
        check_guard(b.in, b.input_bytes);
        check_guard(b.state, b.state_bytes);
        check_guard(b.out, b.output_bytes);
        check_guard(b.scratch, b.scratch_bytes);
        check_guard(b.status, b.status_bytes);
        free_buffers(&b);
    }
    free(reference_out);
    free(reference_state);
}

static void structural_rejections(void)
{
    Buffers b = buffers(2, 1, 1);
    ETAACOfflineParams p = params(&b, 2, 1, 1), bad;
    unsigned char status_before[sizeof(ETAACStatus)];
    fill_input(&b.in[0], 0, 0);
    assert(etaac_offline_valid(&p));
    memcpy(status_before, b.status, sizeof(status_before));

    bad = p;
    bad.generation = UINT64_MAX;
    bad.frames = 2;
    assert(!etaac_offline_valid(&bad));
    assert(etaac_offline_process(&bad, 0) == ETAAC_BAD_PARAMS);

    bad = p;
    bad.frame_offset = 2;
    assert(!etaac_offline_valid(&bad));
    assert(etaac_offline_process(&bad, 0) == ETAAC_BAD_PARAMS);

    bad = p;
    bad.capacity_frames = ETAAC_OFFLINE_MAX_FRAMES + 1;
    assert(!etaac_offline_valid(&bad));
    assert(etaac_offline_process(&bad, 0) == ETAAC_BAD_PARAMS);

    bad = p;
    bad.status_addr = bad.output_addr;
    assert(!etaac_offline_valid(&bad));
    assert(etaac_offline_process(&bad, 0) == ETAAC_BAD_PARAMS);

    bad = p;
    bad.input_addr = 0;
    assert(!etaac_offline_valid(&bad));
    assert(etaac_offline_process(&bad, 0) == ETAAC_BAD_PARAMS);
    assert(!memcmp(status_before, b.status, sizeof(status_before)));
    check_guard(b.in, b.input_bytes);
    check_guard(b.state, b.state_bytes);
    check_guard(b.out, b.output_bytes);
    check_guard(b.scratch, b.scratch_bytes);
    check_guard(b.status, b.status_bytes);
    free_buffers(&b);
}

static void malformed_prefix_poison_and_stale(void)
{
    Buffers b = buffers(4, 1, 1);
    ETAACOfflineParams p = params(&b, 4, 1, 1);
    ETAACState saved;
    unsigned j;

    p.operation = ETAAC_COPY;
    p.frame_offset = 1;
    p.frames = 3;
    p.generation = 5;
    b.state[0].stream_id = 100;
    b.state[0].generation = 4;
    b.state[0].initialized = 1;
    fill_input(&b.in[1], 0, 4);
    b.in[1].stream_id = 100;
    fill_input(&b.in[2], 0, 5);
    b.in[2].stream_id = 100;
    b.in[2].sequence = 4; /* The second requested frame is malformed. */
    assert(etaac_offline_process(&p, 0) == ETAAC_BAD_INPUT);
    assert(b.state[0].initialized == 2 && b.state[0].generation == 5);
    assert(b.state[0].stream_id == 100 && b.state[0].sequence == 0);
    assert(b.status[0].generation == 7 && b.status[0].result == ETAAC_BAD_INPUT);
    assert(b.status[0].samples == ETAAC_SAMPLES);
    for (j = 0; j < ETAAC_SAMPLES; j++)
        assert(b.out[ETAAC_SAMPLES + j] == b.in[1].coeff[j]);

    memcpy(&saved, &b.state[0], sizeof(saved));
    p.frame_offset = 0;
    p.frames = 1;
    p.generation = 6;
    fill_input(&b.in[0], 0, 5);
    b.in[0].stream_id = 100;
    assert(etaac_offline_process(&p, 0) == ETAAC_BAD_STATE);
    assert(!memcmp(&saved, &b.state[0], sizeof(saved)));
    assert(b.status[0].generation == 6 && b.status[0].result == ETAAC_BAD_STATE);
    assert(b.status[0].samples == 0);

    /* A normally initialized but stale slot also cannot be advanced. */
    memset(&b.state[0], 0, sizeof(b.state[0]));
    fill_input(&b.in[0], 0, 0);
    b.in[0].stream_id = 200;
    p.generation = 1;
    assert(!etaac_offline_process(&p, 0));
    memcpy(&saved, &b.state[0], sizeof(saved));
    p.generation = 3;
    fill_input(&b.in[0], 0, 2);
    b.in[0].stream_id = 200;
    assert(etaac_offline_process(&p, 0) == ETAAC_BAD_STATE);
    assert(!memcmp(&saved, &b.state[0], sizeof(saved)));
    free_buffers(&b);
}

static void finite_rejections(void)
{
    Buffers b = buffers(1, 1, 1);
    ETAACOfflineParams p = params(&b, 1, 1, 1);
    uint32_t nan = UINT32_C(0x7fc00000);
    ETAACState before;

    fill_input(&b.in[0], 0, 0);
    memcpy(b.in[0].coeff, &nan, sizeof(nan));
    memcpy(&before, &b.state[0], sizeof(before));
    assert(etaac_offline_process(&p, 0) == ETAAC_BAD_INPUT);
    assert(!memcmp(&before, &b.state[0], sizeof(before)));
    assert(b.status[0].samples == 0 && b.status[0].result == ETAAC_BAD_INPUT);

    memset(&b.state[0], 0, sizeof(b.state[0]));
    b.state[0].stream_id = 99;
    b.state[0].initialized = 1;
    memcpy(b.state[0].saved, &nan, sizeof(nan));
    fill_input(&b.in[0], 0, 0);
    b.in[0].stream_id = 99;
    memcpy(&before, &b.state[0], sizeof(before));
    assert(etaac_offline_process(&p, 0) == ETAAC_BAD_INPUT);
    assert(!memcmp(&before, &b.state[0], sizeof(before)));
    free_buffers(&b);
}

int main(void)
{
    split_launch_equivalence();
    structural_rejections();
    malformed_prefix_poison_and_stale();
    finite_rejections();
    puts("PASS offline native: 64-channel 1/8/32-frame equivalence, ABI bounds, poison/stale and finite checks");
    return 0;
}
