// SPDX-License-Identifier: BSD-2-Clause-Patent
// See AVDEmitter.h.
#include "AVDEmitter.h"
#include "AVDDecoder.h"
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "../../TegraNVDEC/nvidia/nvdec_drv.h"

enum {
    kJobs = 64,  // the session's newest jobs; an older one still unreported is waited for before its record goes
    kEarly = 4,  // completions of jobs submitted and not yet registered (avd_emitter_submitted follows the Submit)
    kPool = 64,  // pooled Output slots (matches kJobs; rare overflow falls back to calloc/free)
};

typedef struct {
    uint32_t fence;
    OSStatus status;
} Early;

typedef struct {
    uint32_t fence;
    bool done;
    OSStatus status;
    TegraNVDECRange range;  // its status record
    const uint8_t *record;
} Job;

struct AVDEmitter {
    _Atomic uint32_t references;  // the decoder's, and a drain's while it calls into VideoToolbox
    _Atomic uint64_t poolInUse;
    _Atomic uint32_t latest;
    _Atomic uint32_t errors;
    _Atomic bool hasReady;
    pthread_mutex_t lock;
    pthread_cond_t emitted;
    dispatch_queue_t queue;
    NVDECClient *client;
    VTVideoDecoderSession session;
    bool closed;
    Job jobs[kJobs];
    Output pool[kPool];
    uint32_t jobHead, jobCount, earlyCount;
    Early early[kEarly];
    uint64_t pushed, emittedThrough;
    Output *unpushed;  // decode-thread-only list of allocated Outputs not yet pushed or settled
    Output *live, *head, *tail;
    pthread_t drainer;
    uint32_t draining, waiters, errorsLogged;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    uint64_t frameBegan;
    uint32_t waits[AVD_ROUTE_ROOM_WAITS + 1], inFlight, inFlightMax;
    uint64_t latencyTicks, latencyCount;
#endif
};

static Output *allocOutput(AVDEmitter *e)
{
    uint64_t cur = atomic_load_explicit(&e->poolInUse, memory_order_acquire);
    for (;;) {
        const uint64_t avail = ~cur;
        if (!avail)
            return calloc(1, sizeof(Output));
        const unsigned slot = (unsigned)__builtin_ctzll(avail);
        if (atomic_compare_exchange_weak_explicit(&e->poolInUse, &cur, cur | (1ULL << slot), memory_order_acq_rel,
                                                  memory_order_acquire)) {
            Output *out = &e->pool[slot];
            memset(out, 0, sizeof(*out));
            return out;
        }
    }
}

static void freeOutput(AVDEmitter *e, Output *out)
{
    if (out >= e->pool && out < e->pool + kPool) {
        const unsigned slot = (unsigned)(out - e->pool);
        atomic_fetch_and_explicit(&e->poolInUse, ~(1ULL << slot), memory_order_release);
        return;
    }
    free(out);
}

static void linkUnpushed(AVDEmitter *e, Output *out)
{
    out->livePrevious = NULL;
    out->liveNext = e->unpushed;
    if (e->unpushed)
        e->unpushed->livePrevious = out;
    e->unpushed = out;
}

static void unlinkUnpushed(AVDEmitter *e, Output *out)
{
    if (out->livePrevious)
        out->livePrevious->liveNext = out->liveNext;
    else if (e->unpushed == out)
        e->unpushed = out->liveNext;
    if (out->liveNext)
        out->liveNext->livePrevious = out->livePrevious;
    out->liveNext = out->livePrevious = NULL;
}

static void count(AVDEmitter *e, unsigned route)
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    e->waits[route]++;
#else
    (void)e, (void)route;
#endif
}

static OSStatus statusOf(int result, uint32_t error, uint32_t correct)
{
    return result ? kVTVideoDecoderMalfunctionErr : error && !correct ? kVTVideoDecoderBadDataErr : noErr;
}

static Job *findJob(AVDEmitter *e, uint32_t fence)
{
    for (uint32_t i = 0; fence && i < e->jobCount; i++) {
        Job *job = &e->jobs[(e->jobHead + i) % kJobs];
        if (job->fence == fence)
            return job;
    }
    return NULL;
}

static bool jobDone(AVDEmitter *e, uint32_t fence, OSStatus *status)
{
    const Job *job = findJob(e, fence);
    if (job && job->done && status)
        *status = job->status;
    return !job || job->done;
}

static bool ready(const Output *out)
{
    return out->jobDone && out->firstFieldDone && out->dueDone;
}

static void markDone(AVDEmitter *e, Job *job, OSStatus status)
{
    job->done = true;
    job->status = status;
    for (Output *out = e->live; out; out = out->liveNext) {
        bool *done = out->job == job->fence             ? &out->jobDone
                     : out->firstFieldJob == job->fence ? &out->firstFieldDone
                                                        : NULL;
        if (done && !*done) {
            *done = true;
            if (status && !out->status) {
                out->status = status;
                atomic_store_explicit(&e->errors, atomic_load_explicit(&e->errors, memory_order_relaxed) + 1,
                                      memory_order_relaxed);
            }
        }
        if (out->due == job->fence)
            out->dueDone = true;
    }
    if (e->head && ready(e->head))
        atomic_store_explicit(&e->hasReady, true, memory_order_release);
}

// Under the lock, which it drops while the kernel waits.
static void settleJob(AVDEmitter *e, Job *job)
{
    const uint32_t fence = job->fence;
    TegraNVDECWait wait = {.fence = fence, .invalidate_count = 1};
    wait.invalidate[0] = job->range;
    const uint8_t *record = job->record;
    pthread_mutex_unlock(&e->lock);
    const int failed = nvdec_wait(e->client, &wait);
    nvdec_status_s status = {0};
    if (!failed && record)
        memcpy(&status, record, sizeof(status));
    pthread_mutex_lock(&e->lock);
    if ((job = findJob(e, fence)) && !job->done)
        markDone(e, job, statusOf(failed, status.error_status, status.mbs_correctly_decoded));
}

static void unlist(AVDEmitter *e, Output *out)
{
    if (out->livePrevious)
        out->livePrevious->liveNext = out->liveNext;
    else if (e->live == out)
        e->live = out->liveNext;
    if (out->liveNext)
        out->liveNext->livePrevious = out->livePrevious;
    out->liveNext = out->livePrevious = NULL;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    e->inFlight--;
#endif
}

static void releaseOutput(AVDEmitter *e, Output *out)
{
    if (out->image)
        CVPixelBufferRelease(out->image);
    freeOutput(e, out);
}

AVDEmitter *avd_emitter_create(NVDECClient *client)
{
    AVDEmitter *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->queue = dispatch_queue_create("com.brunostjohn.switch.avd.completions", DISPATCH_QUEUE_SERIAL);
    if (!e->queue) {
        free(e);
        return NULL;
    }
    pthread_mutex_init(&e->lock, NULL);
    pthread_cond_init(&e->emitted, NULL);
    e->client = client;
    atomic_init(&e->references, 1);
    atomic_init(&e->poolInUse, 0);
    atomic_init(&e->latest, 0);
    atomic_init(&e->errors, 0);
    atomic_init(&e->hasReady, false);
    return e;
}

static void drop(AVDEmitter *e)
{
    if (atomic_fetch_sub_explicit(&e->references, 1, memory_order_acq_rel) != 1)
        return;
    while (e->unpushed) {
        Output *out = e->unpushed;
        unlinkUnpushed(e, out);
        releaseOutput(e, out);
    }
    while (e->live) {
        Output *out = e->live;
        unlist(e, out);
        releaseOutput(e, out);
    }
    dispatch_release(e->queue);
    pthread_cond_destroy(&e->emitted);
    pthread_mutex_destroy(&e->lock);
    free(e);
}

// A client's callback may end the session from inside a drain (invalidating and releasing it): the drain's reference
// keeps the emitter until it returns.
void avd_emitter_destroy(AVDEmitter *e)
{
    if (!e)
        return;
    avd_emitter_close(e);
    drop(e);
}

dispatch_queue_t avd_emitter_queue(const AVDEmitter *e)
{
    return e->queue;
}

// Called with e->lock held; returns with e->lock released.
static void drainLocked(AVDEmitter *e)
{
    const pthread_t self = pthread_self();
    if ((e->draining && !pthread_equal(e->drainer, self)) || !e->head || !ready(e->head)) {
        atomic_store_explicit(&e->hasReady, e->head && ready(e->head), memory_order_release);
        pthread_mutex_unlock(&e->lock);
        return;
    }
    e->draining++;
    e->drainer = self;
    atomic_fetch_add_explicit(&e->references, 1, memory_order_relaxed);
    while (e->head && ready(e->head)) {
        Output *out = e->head;
        e->head = out->next;
        if (!e->head)
            e->tail = NULL;
        unlist(e, out);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        e->latencyTicks += mach_absolute_time() - out->began;
        e->latencyCount++;
#endif
        const VTVideoDecoderSession session = e->closed ? NULL : e->session;
        const uint64_t sequence = out->sequence;
        pthread_mutex_unlock(&e->lock);
        if (session)
            VTDecoderSessionEmitDecodedFrame(session, out->frame, out->status,
                                             out->image ? 0 : kVTDecodeInfo_FrameDropped, out->image);
        releaseOutput(e, out);
        pthread_mutex_lock(&e->lock);
        if (sequence > e->emittedThrough)
            e->emittedThrough = sequence;
    }
    atomic_store_explicit(&e->hasReady, false, memory_order_release);
    e->draining--;
    if (e->waiters)
        pthread_cond_broadcast(&e->emitted);
    pthread_mutex_unlock(&e->lock);
    drop(e);
}

void avd_emitter_completed(void *emitter, uint32_t fence, int result, uint32_t error_status, uint32_t mbs_correct)
{
    AVDEmitter *e = emitter;
    const OSStatus status = statusOf(result, error_status, mbs_correct);
    pthread_mutex_lock(&e->lock);
    // NVDEC's own reading of a picture it could not decode: its error bits and how many macroblocks (CTBs for HEVC) it
    // decoded before them, which places a bitstream fault in the picture; the first few of a session
    if (error_status && e->errorsLogged < 3) {
        e->errorsLogged++;
        os_log_error(gAVDLog, "AVD: picture of fence %u: NVDEC error %#x, %u macroblocks decoded correctly", fence,
                     error_status, mbs_correct);
    }
    Job *job = findJob(e, fence);
    if (job && !job->done) {
        markDone(e, job, status);
    } else if (!job && (int32_t)(fence - atomic_load_explicit(&e->latest, memory_order_relaxed)) > 0) {
        if (e->earlyCount == kEarly)
            memmove(&e->early[0], &e->early[1], sizeof(e->early) - sizeof(e->early[0]));
        e->early[e->earlyCount < kEarly ? e->earlyCount++ : kEarly - 1] = (Early){fence, status};
    }
    drainLocked(e);
}

void avd_emitter_set_session(AVDEmitter *e, VTVideoDecoderSession session)
{
    pthread_mutex_lock(&e->lock);
    e->session = session;
    pthread_mutex_unlock(&e->lock);
}

void avd_emitter_frame_began(AVDEmitter *e, uint64_t now)
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    e->frameBegan = now;
#else
    (void)e, (void)now;
#endif
}

void avd_emitter_submitted(AVDEmitter *e, uint32_t fence, const TegraNVDECSubmit *submit, bool waitedForRoom)
{
    TegraNVDECRange range = {0};
    const bool hasStatus = nvdec_status_range(submit, &range);
    const uint8_t *base = hasStatus ? nvdec_buffer_va(e->client, range.handle) : NULL;
    pthread_mutex_lock(&e->lock);
    if (waitedForRoom)
        count(e, AVD_ROUTE_ROOM_WAITS);
    while (e->jobCount == kJobs && !e->jobs[e->jobHead].done)
        settleJob(e, &e->jobs[e->jobHead]);
    if (e->jobCount == kJobs) {
        e->jobHead = (e->jobHead + 1) % kJobs;
        e->jobCount--;
    }
    Job *job = &e->jobs[(e->jobHead + e->jobCount++) % kJobs];
    *job = (Job){fence, !hasStatus, noErr, range, base ? base + range.offset : NULL};
    atomic_store_explicit(&e->latest, fence, memory_order_relaxed);
    for (uint32_t i = 0; i < e->earlyCount; i++) {
        if (e->early[i].fence != fence)
            continue;
        markDone(e, job, e->early[i].status);
        e->early[i] = e->early[--e->earlyCount];
        break;
    }
    pthread_mutex_unlock(&e->lock);
}

Output *avd_emitter_output(AVDEmitter *e, VTVideoDecoderFrame frame, uint32_t job, uint32_t firstFieldJob)
{
    Output *out = allocOutput(e);
    if (!out)
        return NULL;
    out->frame = frame;
    out->job = job;
    out->firstFieldJob = firstFieldJob;
    out->due = atomic_load_explicit(&e->latest, memory_order_relaxed);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    out->began = e->frameBegan;
#endif
    linkUnpushed(e, out);
    return out;
}

void avd_emitter_fail(AVDEmitter *e, Output *out, OSStatus status)
{
    (void)e;
    out->status = status;
}

void avd_emitter_push(AVDEmitter *e, Output *out)
{
    unlinkUnpushed(e, out);
    pthread_mutex_lock(&e->lock);
    OSStatus status = noErr, firstStatus = noErr;
    out->jobDone = jobDone(e, out->job, &status);
    out->firstFieldDone = jobDone(e, out->firstFieldJob, &firstStatus);
    out->dueDone = jobDone(e, out->due, NULL);
    const OSStatus jobStatus = firstStatus ? firstStatus : status;
    if (jobStatus && !out->status) {
        out->status = jobStatus;
        atomic_store_explicit(&e->errors, atomic_load_explicit(&e->errors, memory_order_relaxed) + 1,
                              memory_order_relaxed);
    }
    out->liveNext = e->live;
    if (e->live)
        e->live->livePrevious = out;
    e->live = out;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (++e->inFlight > e->inFlightMax)
        e->inFlightMax = e->inFlight;
#endif
    out->sequence = ++e->pushed;
    out->next = NULL;
    if (e->tail)
        e->tail->next = out;
    else
        e->head = out;
    e->tail = out;
    if (e->head && ready(e->head))
        atomic_store_explicit(&e->hasReady, true, memory_order_release);
    pthread_mutex_unlock(&e->lock);
}

// One thread emits at a time, so pictures go out in order; the one emitting may do so again from inside a client's
// callback (a synchronous DecodeFrame or an invalidate there), and another finds it busy and leaves the work to it.
void avd_emitter_drain(AVDEmitter *e)
{
    if (!atomic_load_explicit(&e->hasReady, memory_order_acquire))
        return;
    pthread_mutex_lock(&e->lock);
    drainLocked(e);
}

static bool settleJobs(AVDEmitter *e, uint32_t through)
{
    bool waited = false;
    for (uint32_t i = 0; i < e->jobCount; i++) {
        Job *job = &e->jobs[(e->jobHead + i) % kJobs];
        if (!job->done && (int32_t)(through - job->fence) >= 0) {
            settleJob(e, job);
            waited = true;
            i = (uint32_t)-1;
        }
    }
    return waited;
}

void avd_emitter_settle(AVDEmitter *e, bool synchronous)
{
    const pthread_t self = pthread_self();
    pthread_mutex_lock(&e->lock);
    const uint64_t target = e->pushed;
    if (settleJobs(e, atomic_load_explicit(&e->latest, memory_order_relaxed)) && synchronous)
        count(e, AVD_ROUTE_SYNC_WAITS);
    for (;;) {
        if (e->head && ready(e->head) && (!e->draining || pthread_equal(e->drainer, self))) {
            drainLocked(e);
            pthread_mutex_lock(&e->lock);
        }
        while (e->emittedThrough < target && e->draining && !pthread_equal(e->drainer, self)) {
            e->waiters++;
            pthread_cond_wait(&e->emitted, &e->lock);
            e->waiters--;
        }
        if (e->emittedThrough >= target || !e->head ||
            (!ready(e->head) && (!e->draining || pthread_equal(e->drainer, self)))) {
            pthread_mutex_unlock(&e->lock);
            return;
        }
    }
}

OSStatus avd_emitter_settle_output(AVDEmitter *e, Output *out)
{
    unlinkUnpushed(e, out);
    pthread_mutex_lock(&e->lock);
    if (settleJobs(e, out->due))
        count(e, AVD_ROUTE_SYNC_WAITS);
    OSStatus status = noErr, firstStatus = noErr;
    jobDone(e, out->job, &status);
    jobDone(e, out->firstFieldJob, &firstStatus);
    const OSStatus jobStatus = firstStatus ? firstStatus : status;
    if (jobStatus && !out->status) {
        out->status = jobStatus;
        atomic_store_explicit(&e->errors, atomic_load_explicit(&e->errors, memory_order_relaxed) + 1,
                              memory_order_relaxed);
    }
    const OSStatus result = out->status;
    pthread_mutex_unlock(&e->lock);
    releaseOutput(e, out);
    return result;
}

int avd_emitter_wait_picture(AVDEmitter *e, Output *out, TegraNVDECRange range)
{
    pthread_mutex_lock(&e->lock);
    settleJobs(e, out->due);
    count(e, AVD_ROUTE_PICTURE_WAITS);
    const uint32_t due = out->due;
    pthread_mutex_unlock(&e->lock);
    TegraNVDECWait wait = {.fence = due, .invalidate_count = 1};
    wait.invalidate[0] = range;
    return due ? nvdec_wait(e->client, &wait) : 0;
}

void avd_emitter_dependency_wait(AVDEmitter *e)
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    pthread_mutex_lock(&e->lock);
    count(e, AVD_ROUTE_DEPENDENCY_WAITS);
    pthread_mutex_unlock(&e->lock);
#else
    (void)e;
#endif
}

void avd_emitter_close(AVDEmitter *e)
{
    const pthread_t self = pthread_self();
    pthread_mutex_lock(&e->lock);
    e->closed = true;
    e->session = NULL;
    while (e->draining && !pthread_equal(e->drainer, self)) {
        e->waiters++;
        pthread_cond_wait(&e->emitted, &e->lock);
        e->waiters--;
    }
    pthread_mutex_unlock(&e->lock);
}

uint32_t avd_emitter_errors(const AVDEmitter *e)
{
    return atomic_load_explicit(&e->errors, memory_order_relaxed);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
void avd_emitter_routes(const AVDEmitter *e, int32_t counts[AVD_ROUTE_COUNT])
{
    static mach_timebase_info_data_t base;
    if (!base.denom)
        mach_timebase_info(&base);
    for (unsigned i = 0; i <= AVD_ROUTE_ROOM_WAITS; i++)
        counts[i] = (int32_t)e->waits[i];
    counts[AVD_ROUTE_IN_FLIGHT_MAX] = (int32_t)e->inFlightMax;
    counts[AVD_ROUTE_EMIT_LATENCY_US] =
        e->latencyCount ? (int32_t)(e->latencyTicks * base.numer / base.denom / 1000 / e->latencyCount) : 0;
}
#endif
