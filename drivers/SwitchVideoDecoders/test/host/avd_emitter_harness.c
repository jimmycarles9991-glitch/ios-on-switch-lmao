// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "avd_emitter_harness.h"
#include <CoreVideo/CoreVideo.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "../../AVD/AVDDecoder.h"
#include "../../../TegraNVDEC/nvidia/nvdec_drv.h"

os_log_t gAVDLog;

static _Atomic uint32_t gLocks;
static _Atomic uint32_t gBroadcasts;
static _Atomic uint32_t gOutputAllocs;
static _Atomic uint32_t gEmittedCount;
static _Atomic uint32_t gWaitCalls;
static _Atomic int32_t gLastStatus;
static _Atomic(void *) gLastFrame;

static void (*gEmitHook)(void *frame, OSStatus status, void *ctx);
static void *gEmitHookCtx;
static int (*gWaitHook)(uint32_t fence, void *ctx);
static void *gWaitHookCtx;
static int gWaitRet;
static uint32_t gWaitErrStatus, gWaitMbsCorrect;
static uint32_t gStatusHandle;
static uint8_t *gStatusVa;

static int harness_mutex_lock(pthread_mutex_t *m)
{
    atomic_fetch_add_explicit(&gLocks, 1, memory_order_relaxed);
    return pthread_mutex_lock(m);
}

static int harness_cond_broadcast(pthread_cond_t *c)
{
    atomic_fetch_add_explicit(&gBroadcasts, 1, memory_order_relaxed);
    return pthread_cond_broadcast(c);
}

static void *harness_calloc(size_t count, size_t size)
{
    if (count == 1 && size == sizeof(Output))
        atomic_fetch_add_explicit(&gOutputAllocs, 1, memory_order_relaxed);
    return calloc(count, size);
}

OSStatus VTDecoderSessionEmitDecodedFrame(VTVideoDecoderSession session, VTVideoDecoderFrame frame, OSStatus status,
                                          VTDecodeInfoFlags info, CVImageBufferRef image)
{
    (void)session, (void)info, (void)image;
    atomic_fetch_add_explicit(&gEmittedCount, 1, memory_order_relaxed);
    atomic_store_explicit(&gLastStatus, status, memory_order_relaxed);
    atomic_store_explicit(&gLastFrame, frame, memory_order_relaxed);
    if (gEmitHook)
        gEmitHook(frame, status, gEmitHookCtx);
    return noErr;
}

void *nvdec_buffer_va(const NVDECClient *client, uint32_t handle)
{
    (void)client;
    return handle && handle == gStatusHandle ? gStatusVa : NULL;
}

int nvdec_wait(void *client, const TegraNVDECWait *wait)
{
    (void)client;
    atomic_fetch_add_explicit(&gWaitCalls, 1, memory_order_relaxed);
    const int ret = gWaitHook ? gWaitHook(wait->fence, gWaitHookCtx) : gWaitRet;
    if (!ret && wait->invalidate_count && wait->invalidate[0].handle == gStatusHandle && gStatusVa) {
        nvdec_status_s st = {0};
        st.error_status = gWaitErrStatus;
        st.mbs_correctly_decoded = gWaitMbsCorrect;
        memcpy(gStatusVa + wait->invalidate[0].offset, &st, sizeof(st));
    }
    return ret;
}

#define pthread_mutex_lock(m)     harness_mutex_lock(m)
#define pthread_cond_broadcast(c) harness_cond_broadcast(c)
#define calloc(n, s)              harness_calloc(n, s)
#include "../../AVD/AVDEmitter.c"
#undef pthread_mutex_lock
#undef pthread_cond_broadcast
#undef calloc

void avd_emitter_harness_reset(void)
{
    if (!gAVDLog)
        gAVDLog = os_log_create("com.brunostjohn.switch.avd", "emitter-test");
    atomic_store(&gLocks, 0);
    atomic_store(&gBroadcasts, 0);
    atomic_store(&gOutputAllocs, 0);
    atomic_store(&gEmittedCount, 0);
    atomic_store(&gWaitCalls, 0);
    atomic_store(&gLastStatus, 0);
    atomic_store(&gLastFrame, NULL);
    gEmitHook = NULL;
    gEmitHookCtx = NULL;
    gWaitHook = NULL;
    gWaitHookCtx = NULL;
    gWaitRet = 0;
    gWaitErrStatus = 0;
    gWaitMbsCorrect = 0;
    gStatusHandle = 0;
    gStatusVa = NULL;
}

AVDEmitterHarnessStats avd_emitter_harness_stats(void)
{
    AVDEmitterHarnessStats s;
    s.lockAcquisitions = atomic_load(&gLocks);
    s.broadcasts = atomic_load(&gBroadcasts);
    s.outputHeapAllocs = atomic_load(&gOutputAllocs);
    s.emittedCount = atomic_load(&gEmittedCount);
    s.waitCalls = atomic_load(&gWaitCalls);
    s.lastEmittedStatus = atomic_load(&gLastStatus);
    s.lastEmittedFrame = atomic_load(&gLastFrame);
    return s;
}

void avd_emitter_harness_set_emit_hook(void (*hook)(void *frame, OSStatus status, void *ctx), void *ctx)
{
    gEmitHook = hook;
    gEmitHookCtx = ctx;
}

void avd_emitter_harness_set_wait_hook(int (*hook)(uint32_t fence, void *ctx), void *ctx)
{
    gWaitHook = hook;
    gWaitHookCtx = ctx;
}

void avd_emitter_harness_set_wait_status(int ret, uint32_t error_status, uint32_t mbs_correct)
{
    gWaitRet = ret;
    gWaitErrStatus = error_status;
    gWaitMbsCorrect = mbs_correct;
}

void avd_emitter_harness_set_status_buffer(uint32_t handle, void *va)
{
    gStatusHandle = handle;
    gStatusVa = va;
}

struct AVDGateHarness {
    Decoder d;
    _Atomic uint32_t slowLocks;
};

void avd_decoder_enter_slow(Decoder *d)
{
    AVDGateHarness *g = (AVDGateHarness *)d;
    atomic_fetch_add_explicit(&g->slowLocks, 1, memory_order_relaxed);
    pthread_mutex_lock(&d->lock);
    d->waiters++;
    for (;;) {
        uint32_t cur = atomic_load_explicit(&d->owner, memory_order_relaxed);
        if (cur == 0) {
            const uint32_t next = d->waiters > 1 ? 2u : 1u;
            if (atomic_compare_exchange_weak_explicit(&d->owner, &cur, next, memory_order_acquire,
                                                      memory_order_relaxed))
                break;
            continue;
        }
        if (cur == 1u &&
            !atomic_compare_exchange_weak_explicit(&d->owner, &cur, 2u, memory_order_relaxed, memory_order_relaxed))
            continue;
        pthread_cond_wait(&d->idle, &d->lock);
    }
    d->waiters--;
    pthread_mutex_unlock(&d->lock);
}

void avd_decoder_leave_slow(Decoder *d)
{
    AVDGateHarness *g = (AVDGateHarness *)d;
    atomic_fetch_add_explicit(&g->slowLocks, 1, memory_order_relaxed);
    pthread_mutex_lock(&d->lock);
    atomic_store_explicit(&d->owner, 0u, memory_order_release);
    if (d->waiters)
        pthread_cond_signal(&d->idle);
    pthread_mutex_unlock(&d->lock);
}

AVDGateHarness *avd_gate_harness_create(void)
{
    AVDGateHarness *g = calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    pthread_mutex_init(&g->d.lock, NULL);
    pthread_cond_init(&g->d.idle, NULL);
    atomic_init(&g->d.owner, 0);
    atomic_init(&g->slowLocks, 0);
    return g;
}

void avd_gate_harness_destroy(AVDGateHarness *g)
{
    if (!g)
        return;
    pthread_cond_destroy(&g->d.idle);
    pthread_mutex_destroy(&g->d.lock);
    free(g);
}

void avd_gate_harness_enter(AVDGateHarness *g)
{
    avd_decoder_enter(&g->d);
}

void avd_gate_harness_leave(AVDGateHarness *g)
{
    avd_decoder_leave(&g->d);
}

uint32_t avd_gate_harness_slow_locks(const AVDGateHarness *g)
{
    return atomic_load_explicit(&g->slowLocks, memory_order_relaxed);
}
