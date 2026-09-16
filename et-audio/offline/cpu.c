/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Offline AAC-LC IMDCT/window CPU baseline.  This replays captured synthesis
 * records; it is deliberately not an end-to-end AAC decode benchmark.
 */
#define _GNU_SOURCE
#include "../capture_io.h"
#include "../dsp/synth.h"
#include "../protocol.h"
#include "libavcodec/aactab.h"
#include "libavcodec/sinewin.h"
#include "libavutil/cpu.h"
#include "libavutil/float_dsp.h"
#include "libavutil/mem.h"
#include "libavutil/tx.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cpu_reference_synth.h"

#define ETAAC_CPU_ALIGNMENT 64u

_Static_assert(offsetof(ETAACInput, coeff) % ETAAC_CPU_ALIGNMENT == 0,
               "ETAACInput coefficient copy must be cache-line aligned");
_Static_assert(offsetof(ETAACState, saved) % ETAAC_CPU_ALIGNMENT == 0,
               "ETAACState overlap copy must be cache-line aligned");
_Static_assert(sizeof(ETAACInput) % ETAAC_CPU_ALIGNMENT == 0 &&
               sizeof(ETAACState) % ETAAC_CPU_ALIGNMENT == 0,
               "frame/channel strides must preserve alignment");

typedef struct Worker {
    unsigned index, channels, first_channel, last_channel;
    int cpu, affinity_error, init_error;
    AVTXContext *tx1024, *tx128;
    av_tx_fn fn1024, fn128;
    AVFloatDSPContext *fdsp;
    _Alignas(64) float coeff[ETAAC_SAMPLES];
    _Alignas(64) float saved[ETAAC_SAVED];
    _Alignas(64) float temp[ETAAC_SAMPLES];
    _Alignas(64) float window_temp[128];
    _Alignas(64) float output[ETAAC_SAMPLES];
} Worker;

typedef struct Run {
    unsigned channels, frames;
    ETAACInput *inputs;
    ETAACState *initial_states;
    float *output; /* frame-major: [frame][channel][1024] */
    pthread_barrier_t start, done;
    pthread_mutex_t startup_lock;
    pthread_cond_t startup_cond;
    unsigned startup_count;
    int startup_release;
    _Atomic int stop;
    Worker *workers;
} Run;

typedef struct Differences {
    uint64_t pcm_different, state_different, nonfinite;
    double pcm_max_abs, state_max_abs;
} Differences;

static double now_seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void *aligned_zero_alloc(size_t bytes)
{
    void *p;
    if (!bytes || bytes % ETAAC_CPU_ALIGNMENT)
        return NULL;
    p = aligned_alloc(ETAAC_CPU_ALIGNMENT, bytes);
    if (p)
        memset(p, 0, bytes);
    return p;
}

static int pin_current_thread(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

static void synth_channel(Worker *w, const Run *run, unsigned channel)
{
    unsigned previous_sequence = run->initial_states[channel].sequence;
    unsigned previous_shape = run->initial_states[channel].shape;

    memcpy(w->saved, run->initial_states[channel].saved, sizeof(w->saved));
    for (unsigned frame = 0; frame < run->frames; frame++) {
        const ETAACInput *in = &run->inputs[(size_t)frame * run->channels + channel];
        float *dst = run->output + ((size_t)frame * run->channels + channel) * ETAAC_SAMPLES;

        /* This aligned copy and the full frame-major PCM store are in timing. */
        memcpy(w->coeff, in->coeff, sizeof(w->coeff));
        reference_synth(w->output, w->saved, w->coeff, in->sequence,
                        previous_sequence, in->shape, previous_shape,
                        w->tx1024, w->fn1024, w->tx128, w->fn128, w->fdsp,
                        w->temp, w->window_temp);
        memcpy(dst, w->output, sizeof(w->output));
        previous_sequence = in->sequence;
        previous_shape = in->shape;
    }
}

typedef struct WorkerStart {
    Run *run;
    Worker *worker;
} WorkerStart;

static void *worker_start(void *opaque)
{
    WorkerStart *start = opaque;
    Run *run = start->run;
    Worker *w = start->worker;

    w->affinity_error = pin_current_thread(w->cpu);
    pthread_mutex_lock(&run->startup_lock);
    run->startup_count++;
    /* Main and workers wait on different predicates of this condition;
     * signal could wake another worker and strand main after the last arrival. */
    pthread_cond_broadcast(&run->startup_cond);
    while (!run->startup_release)
        pthread_cond_wait(&run->startup_cond, &run->startup_lock);
    pthread_mutex_unlock(&run->startup_lock);
    if (atomic_load_explicit(&run->stop, memory_order_acquire))
        return NULL;
    for (;;) {
        (void)pthread_barrier_wait(&run->start);
        if (atomic_load_explicit(&run->stop, memory_order_acquire))
            break;
        for (unsigned channel = w->first_channel; channel < w->last_channel; channel++)
            synth_channel(w, run, channel);
        (void)pthread_barrier_wait(&run->done);
    }
    return NULL;
}

static int init_worker(Worker *w, unsigned index, unsigned workers)
{
    float scale1024 = (1.0f / 1024.0f) / 32768.0f;
    float scale128 = (1.0f / 128.0f) / 32768.0f;

    memset(w, 0, sizeof(*w));
    w->index = index;
    w->cpu = (int)(2 * index); /* Observed physical P-core sequence: 0,2,...,14. */
    if (av_tx_init(&w->tx1024, &w->fn1024, AV_TX_FLOAT_MDCT, 1, 1024,
                   &scale1024, 0) < 0 ||
        av_tx_init(&w->tx128, &w->fn128, AV_TX_FLOAT_MDCT, 1, 128,
                   &scale128, 0) < 0 ||
        !(w->fdsp = avpriv_float_dsp_alloc(0))) {
        w->init_error = 1;
        return -1;
    }
    (void)workers;
    return 0;
}

static void free_worker(Worker *w)
{
    av_tx_uninit(&w->tx1024);
    av_tx_uninit(&w->tx128);
    av_free(w->fdsp);
    memset(w, 0, sizeof(*w));
}

static void add_difference(Differences *d, float got, float want, int state)
{
    if (!isfinite(got) || !isfinite(want)) {
        d->nonfinite++;
        return;
    }
    if (memcmp(&got, &want, sizeof(got))) {
        double error = fabs((double)got - (double)want);
        if (state) {
            d->state_different++;
            if (error > d->state_max_abs)
                d->state_max_abs = error;
        } else {
            d->pcm_different++;
            if (error > d->pcm_max_abs)
                d->pcm_max_abs = error;
        }
    }
}

/* Validation is intentionally sequential and completes before thread launch. */
static Differences validate_capture(const Run *run, Worker *w, const ETAACCapture *capture)
{
    Differences d = {0};

    for (unsigned channel = 0; channel < run->channels; channel++) {
        unsigned previous_sequence = run->initial_states[channel].sequence;
        unsigned previous_shape = run->initial_states[channel].shape;
        const ETAACRecord *last = NULL;

        memcpy(w->saved, run->initial_states[channel].saved, sizeof(w->saved));
        for (unsigned frame = 0; frame < run->frames; frame++) {
            const ETAACInput *in = &run->inputs[(size_t)frame * run->channels + channel];
            const ETAACRecord *record = capture_record(capture, channel, frame);
            memcpy(w->coeff, in->coeff, sizeof(w->coeff));
            reference_synth(w->output, w->saved, w->coeff, in->sequence,
                            previous_sequence, in->shape, previous_shape,
                            w->tx1024, w->fn1024, w->tx128, w->fn128, w->fdsp,
                            w->temp, w->window_temp);
            memcpy(run->output + ((size_t)frame * run->channels + channel) * ETAAC_SAMPLES,
                   w->output, sizeof(w->output));
            for (unsigned sample = 0; sample < ETAAC_SAMPLES; sample++)
                add_difference(&d, w->output[sample], record->output[sample], 0);
            previous_sequence = in->sequence;
            previous_shape = in->shape;
            last = record;
        }
        for (unsigned sample = 0; sample < ETAAC_SAVED; sample++)
            add_difference(&d, w->saved[sample], last->after[sample], 1);
    }
    return d;
}

static uint64_t pcm_checksum(const float *pcm, size_t samples)
{
    uint64_t h = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < samples; i++) {
        uint32_t bits;
        memcpy(&bits, pcm + i, sizeof(bits));
        h ^= bits;
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static void print_json_string(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            putchar('\\');
        putchar(*s);
    }
    putchar('"');
}

int main(int argc, char **argv)
{
    ETAACCapture capture = {0};
    Run run = {0};
    Worker workers[8] = {{0}};
    WorkerStart starts[8];
    pthread_t threads[8];
    double *samples = NULL, pack_started, preprocess_s;
    uint64_t *checksums = NULL;
    uint64_t reference_checksum = 0;
    unsigned channels, frames, worker_count, repeats;
    unsigned created = 0;
    int barriers_initialized = 0, workers_released = 0;
    int scalar, result = 1;
    Differences differences;

    if ((argc != 6 && argc != 7) || (argc == 7 && strcmp(argv[6], "scalar"))) {
        fprintf(stderr, "usage: cpu-offline CAPTURE CHANNELS FRAMES WORKERS REPEATS [scalar]\\n");
        return 2;
    }
    channels = (unsigned)strtoul(argv[2], NULL, 10);
    frames = (unsigned)strtoul(argv[3], NULL, 10);
    worker_count = (unsigned)strtoul(argv[4], NULL, 10);
    repeats = (unsigned)strtoul(argv[5], NULL, 10);
    scalar = argc == 7;
    if ((channels != 2 && channels != 64) || frames != 512 ||
        (worker_count != 1 && worker_count != 8) || !repeats || repeats > 10000) {
        fprintf(stderr, "CHANNELS must be 2 or 64; FRAMES must be 512; WORKERS must be 1 or 8\\n");
        return 2;
    }
    if (scalar)
        av_force_cpu_flags(0);
    if (capture_load(&capture, argv[1]) || capture_select(&capture, frames)) {
        fprintf(stderr, "could not load %u-frame captured AAC timeline: %s\\n", frames, argv[1]);
        goto done;
    }

    /* Packing is distinct from dispatch/table setup and every timed replay. */
    pack_started = now_seconds();
    run.channels = channels;
    run.frames = frames;
    run.inputs = aligned_zero_alloc((size_t)frames * channels * sizeof(*run.inputs));
    run.initial_states = aligned_zero_alloc((size_t)channels * sizeof(*run.initial_states));
    run.output = aligned_zero_alloc((size_t)frames * channels * ETAAC_SAMPLES * sizeof(*run.output));
    samples = calloc(repeats, sizeof(*samples));
    checksums = calloc(repeats, sizeof(*checksums));
    if (!run.inputs || !run.initial_states || !run.output || !samples || !checksums) {
        fprintf(stderr, "allocation failed\\n");
        goto done;
    }
    for (unsigned channel = 0; channel < channels; channel++) {
        ETAACState *state = run.initial_states + channel;
        const ETAACRecord *first = capture_record(&capture, channel, 0);
        state->stream_id = channel + 1;
        state->generation = 0;
        state->sequence = first->previous_sequence;
        state->shape = first->previous_shape;
        state->initialized = 1;
        memcpy(state->saved, first->before, sizeof(state->saved));
        for (unsigned frame = 0; frame < frames; frame++) {
            ETAACInput *in = run.inputs + (size_t)frame * channels + channel;
            const ETAACRecord *record = capture_record(&capture, channel, frame);
            in->stream_id = channel + 1;
            in->sequence = record->sequence;
            in->shape = record->shape;
            in->expected_generation = frame;
            memcpy(in->coeff, record->coeff, sizeof(in->coeff));
        }
    }
    preprocess_s = now_seconds() - pack_started;

    /* Global AAC tables and all optimized dispatch contexts are initialized
     * serially before any worker exists; neither is included in measurements. */
    ff_aac_float_common_init();
    for (unsigned worker = 0; worker < worker_count; worker++) {
        if (init_worker(&workers[worker], worker, worker_count)) {
            fprintf(stderr, "FFmpeg dispatch initialization failed for worker %u\\n", worker);
            goto done;
        }
        workers[worker].channels = channels;
        workers[worker].first_channel = channels * worker / worker_count;
        workers[worker].last_channel = channels * (worker + 1) / worker_count;
    }

    differences = validate_capture(&run, &workers[0], &capture);
    reference_checksum = pcm_checksum(run.output, (size_t)frames * channels * ETAAC_SAMPLES);
    if (differences.nonfinite || (scalar &&
        (differences.pcm_different || differences.state_different))) {
        fprintf(stderr, "%s capture validation failed: pcm=%" PRIu64 " state=%" PRIu64
                " nonfinite=%" PRIu64 "\\n", scalar ? "scalar exact" : "optimized",
                differences.pcm_different, differences.state_different, differences.nonfinite);
        goto done;
    }

    {
        int affinity_error = pin_current_thread(0);
        if (affinity_error) {
            fprintf(stderr, "could not pin main thread to CPU 0: %s\\n", strerror(affinity_error));
            goto done;
        }
    }
    if (pthread_barrier_init(&run.start, NULL, worker_count + 1) ||
        pthread_barrier_init(&run.done, NULL, worker_count + 1) ||
        pthread_mutex_init(&run.startup_lock, NULL) ||
        pthread_cond_init(&run.startup_cond, NULL)) {
        fprintf(stderr, "pthread synchronization initialization failed\\n");
        goto done;
    }
    barriers_initialized = 1;
    run.workers = workers;
    for (unsigned worker = 0; worker < worker_count; worker++) {
        starts[worker].run = &run;
        starts[worker].worker = &workers[worker];
        if (pthread_create(&threads[worker], NULL, worker_start, &starts[worker])) {
            fprintf(stderr, "pthread_create failed\\n");
            atomic_store(&run.stop, 1);
            break;
        }
        created++;
    }
    if (created != worker_count)
        goto stop_threads;
    pthread_mutex_lock(&run.startup_lock);
    while (run.startup_count != worker_count)
        pthread_cond_wait(&run.startup_cond, &run.startup_lock);
    pthread_mutex_unlock(&run.startup_lock);
    for (unsigned worker = 0; worker < worker_count; worker++) {
        if (workers[worker].affinity_error) {
            fprintf(stderr, "could not pin worker %u to CPU %d: %s\\n", worker,
                    workers[worker].cpu, strerror(workers[worker].affinity_error));
            goto stop_threads;
        }
    }
    pthread_mutex_lock(&run.startup_lock);
    run.startup_release = 1;
    workers_released = 1;
    pthread_cond_broadcast(&run.startup_cond);
    pthread_mutex_unlock(&run.startup_lock);

    for (unsigned repeat = 0; repeat < repeats; repeat++) {
        const double started = now_seconds();
        /* Timing includes reusable-worker start/completion barriers, aligned
         * coefficient copies, synthesis, and full frame-major PCM stores. */
        (void)pthread_barrier_wait(&run.start);
        (void)pthread_barrier_wait(&run.done);
        samples[repeat] = now_seconds() - started;
        checksums[repeat] = pcm_checksum(run.output,
            (size_t)frames * channels * ETAAC_SAMPLES);
        if (checksums[repeat] != reference_checksum) {
            fprintf(stderr, "parallel PCM differs from serial CPU reference\n");
            goto stop_threads;
        }
    }
    result = 0;

stop_threads:
    atomic_store_explicit(&run.stop, 1, memory_order_release);
    if (barriers_initialized) {
        pthread_mutex_lock(&run.startup_lock);
        run.startup_release = 1;
        pthread_cond_broadcast(&run.startup_cond);
        pthread_mutex_unlock(&run.startup_lock);
    }
    if (created && workers_released)
        (void)pthread_barrier_wait(&run.start);
    for (unsigned worker = 0; worker < created; worker++)
        (void)pthread_join(threads[worker], NULL);
    if (barriers_initialized) {
        pthread_cond_destroy(&run.startup_cond);
        pthread_mutex_destroy(&run.startup_lock);
        pthread_barrier_destroy(&run.done);
        pthread_barrier_destroy(&run.start);
    }

    if (!result) {
        printf("{\"type\":\"cpu_offline_aac_synth\",\"scope\":");
        print_json_string("captured AAC-LC IMDCT/window synthesis replay only; not end-to-end audio decode");
        printf(",\"scalar\":%d,\"channels\":%u,\"frames\":%u,\"workers\":%u,"
               "\"repeats\":%u,\"cpu_flags\":%d,\"affinity\":\"main=0; workers=0,2,...,14\","
               "\"preprocess_pack_s\":%.9f,"
               "\"timing_scope\":\"reusable start barrier through completion barrier; thread creation and dispatch/table initialization excluded\","
               "\"optimized_comparison_characterization_only\":%d,"
               "\"pcm_different_samples\":%" PRIu64 ",\"pcm_max_abs_error\":%.17g,"
               "\"final_state_different_samples\":%" PRIu64 ",\"final_state_max_abs_error\":%.17g,"
               "\"nonfinite_samples\":%" PRIu64 ",\"samples\":[",
               scalar, channels, frames, worker_count, repeats, av_get_cpu_flags(), preprocess_s,
               !scalar, differences.pcm_different, differences.pcm_max_abs,
               differences.state_different, differences.state_max_abs, differences.nonfinite);
        for (unsigned repeat = 0; repeat < repeats; repeat++)
            printf("%s{\"elapsed_s\":%.9f,\"checksum\":\"%016" PRIx64 "\"}",
                   repeat ? "," : "", samples[repeat], checksums[repeat]);
        puts("]}");
    }

done:
    for (unsigned worker = 0; worker < worker_count && worker < 8; worker++)
        free_worker(&workers[worker]);
    free((void *)run.inputs);
    free((void *)run.initial_states);
    free(run.output);
    free(samples);
    free(checksums);
    capture_free(&capture);
    return result;
}
