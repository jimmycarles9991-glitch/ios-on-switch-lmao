// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/thread.h>
#include "SwitchVICEvents.h"

struct SwitchVICEvents {
    IOLock *lock;
    uint32_t refs, waiting, tokenCount;
    switch_gpu_event *events[2];
    switch_gpu_event_watch *watches[2];
    switch_gpu_event_signal *signals[2];
    switch_gpu_event_use *uses[2];
    switch_gpu_backing_token tokens[2];
    switch_gpu_event_waits *batch;
    uint64_t values[2];
    void (*ready)(void *, int);
    void *context;
    int status;
    bool arming, notified, notifying, prepared, committed;
};

namespace {
void drop(SwitchVICEvents *work)
{
    if (__atomic_sub_fetch(&work->refs, 1u, __ATOMIC_ACQ_REL))
        return;
    for (uint32_t i = 0; i < 2; ++i) {
        switch_gpu_event_signal_abort(&work->signals[i]);
        switch_gpu_event_use_release(&work->uses[i]);
        switch_gpu_event_release(&work->events[i]);
    }
    if (work->committed)
        switch_gpu_event_waits_release(&work->batch);
    else
        switch_gpu_event_waits_abort(&work->batch);
    IOLockFree(work->lock);
    IODelete(work, SwitchVICEvents, 1);
}

void notice(SwitchVICEvents *work)
{
    IOLockLock(work->lock);
    const bool notify = !work->arming && !work->waiting && !work->notified;
    work->notified |= notify;
    work->notifying |= notify;
    const int status = work->status;
    IOLockUnlock(work->lock);
    if (notify) {
        work->ready(work->context, status);
        IOLockLock(work->lock);
        work->notifying = false;
        IOLockWakeup(work->lock, &work->notifying, false);
        IOLockUnlock(work->lock);
    }
}

void observed(void *context, uint64_t index, int status)
{
    auto *work = static_cast<SwitchVICEvents *>(context);
    IOLockLock(work->lock);
    work->waiting &= ~(1u << index);
    if (status)
        work->status = status;
    IOLockUnlock(work->lock);
    notice(work);
    drop(work);
}
}

int switch_vic_events_create(void *task, const TegraVICRequest &request, void (*ready)(void *, int), void *context,
                             SwitchVICEvents **out)
{
    *out = nullptr;
    if (!request.src_event_port && !request.dst_event_port)
        return 0;
    if (!ready)
        return -22;
    auto *work = IONewZero(SwitchVICEvents, 1);
    if (!work)
        return -12;
    work->refs = 1;
    work->lock = IOLockAlloc();
    if (!work->lock) {
        IODelete(work, SwitchVICEvents, 1);
        return -12;
    }
    work->ready = ready;
    work->context = context;
    work->values[0] = request.src_wait;
    work->values[1] = request.dst_wait;
    const uint32_t ports[] = {request.src_event_port, request.dst_event_port};
    const uint64_t signals[] = {request.src_signal, request.dst_signal};
    int error = 0;
    for (uint32_t i = 0; i < 2 && !error; ++i) {
        if (!ports[i])
            continue;
        error = switch_gpu_event_acquire(task, ports[i], &work->events[i]);
        if (!error && signals[i])
            error = switch_gpu_event_signal_prepare(work->events[i], signals[i], &work->signals[i]);
    }
    if (error)
        drop(work);
    else
        *out = work;
    return error;
}

void switch_vic_events_arm(SwitchVICEvents *work)
{
    if (!work)
        return;
    IOLockLock(work->lock);
    work->arming = true;
    IOLockUnlock(work->lock);
    for (uint32_t i = 0; i < 2; ++i) {
        if (!work->events[i])
            continue;
        switch_gpu_backing_token producer = {};
        const int resolved = switch_gpu_event_resolve(work->events[i], work->values[i], &producer);
        if (resolved == 2)
            switch_gpu_backing_token_release(&producer);
        if (resolved) {
            if (resolved < 0) {
                IOLockLock(work->lock);
                work->status = resolved;
                IOLockUnlock(work->lock);
            }
            continue;
        }
        IOLockLock(work->lock);
        work->waiting |= 1u << i;
        IOLockUnlock(work->lock);
        __atomic_add_fetch(&work->refs, 1u, __ATOMIC_RELAXED);
        const int error =
            switch_gpu_event_observe(work->events[i], work->values[i], i, observed, work, &work->watches[i]);
        if (error)
            observed(work, i, error);
    }
    IOLockLock(work->lock);
    work->arming = false;
    IOLockUnlock(work->lock);
    notice(work);
}

void switch_vic_events_release(SwitchVICEvents **owned)
{
    if (!owned || !*owned)
        return;
    auto *work = *owned;
    *owned = nullptr;
    IOLockLock(work->lock);
    work->notified = true;
    while (work->notifying)
        IOLockSleep(work->lock, &work->notifying, THREAD_UNINT);
    IOLockUnlock(work->lock);
    for (auto &watch : work->watches)
        switch_gpu_event_observe_cancel(&watch);
    drop(work);
}

int switch_vic_events_prepare(SwitchVICEvents *work)
{
    if (!work || work->prepared)
        return 0;
    uint32_t used = 0;
    for (uint32_t i = 0; i < 2; ++i) {
        if (!work->events[i])
            continue;
        switch_gpu_backing_token token = {};
        const int resolved = switch_gpu_event_resolve_use(work->events[i], work->values[i], &token, &work->uses[used]);
        if (resolved <= 0)
            return resolved ? resolved : -19;
        if (resolved == 2) {
            work->tokens[used] = token;
            ++used;
        }
    }
    work->tokenCount = used;
    const int error = switch_gpu_event_waits_prepare(work->uses, used, &work->batch);
    work->prepared = error == 0;
    return error;
}

uint32_t switch_vic_events_tokens(SwitchVICEvents *work, const switch_gpu_backing_token **tokens)
{
    *tokens = work ? work->tokens : nullptr;
    return work ? work->tokenCount : 0;
}

switch_gpu_event_waits *switch_vic_events_batch(SwitchVICEvents *work)
{
    return work ? work->batch : nullptr;
}

int switch_vic_events_bind(SwitchVICEvents *work, const switch_gpu_backing_token *token)
{
    if (!work)
        return 0;
    for (auto *signal : work->signals) {
        if (!signal)
            continue;
        int error = switch_gpu_event_signal_inherit(signal, work->batch);
        if (!error)
            error = switch_gpu_event_signal_bind(signal, token);
        if (error)
            return error;
    }
    return 0;
}

void switch_vic_events_commit(SwitchVICEvents *work)
{
    if (!work)
        return;
    work->committed = true;
    for (auto &signal : work->signals)
        switch_gpu_event_signal_commit(&signal);
}

void switch_vic_events_submission(SwitchVICEvents *work, bool final, switch_gpu_event_submission *out)
{
    *out = {};
    if (!work)
        return;
    out->dependencies = work->tokens;
    out->dependency_count = work->tokenCount;
    if (!final)
        return;
    out->waits = work->batch;
    if (!work->signals[0]) {
        work->signals[0] = work->signals[1];
        work->signals[1] = nullptr;
    }
    out->signals = work->signals;
    out->signal_count = work->signals[1] ? 2u : work->signals[0] ? 1u : 0u;
}

int switch_vic_events_stage(SwitchVICEvents *source, SwitchVICEvents **out)
{
    *out = nullptr;
    if (!source)
        return 0;
    auto *work = IONewZero(SwitchVICEvents, 1);
    if (!work)
        return -12;
    work->refs = 1;
    work->lock = IOLockAlloc();
    if (!work->lock) {
        IODelete(work, SwitchVICEvents, 1);
        return -12;
    }
    const int error = switch_gpu_event_waits_prepare(source->uses, source->tokenCount, &work->batch);
    if (error) {
        drop(work);
        return error;
    }
    work->prepared = true;
    work->tokenCount = source->tokenCount;
    memcpy(work->tokens, source->tokens, sizeof(work->tokens));
    *out = work;
    return 0;
}
