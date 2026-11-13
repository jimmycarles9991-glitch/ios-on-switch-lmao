// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/thread.h>
#include <string.h>
#include "SwitchVICJobs.h"
#include "SwitchVICDependencies.h"
#include "SwitchVICEvents.h"
#include "SwitchVICSemantics.h"
#include "../TegraHost1x/SwitchHost1xResults.h"
#include "../TegraHost1x/SwitchHost1xStages.h"
#include "../TegraHost1x/TegraHost1x.h"

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
namespace {
const char *gLastRefusal;
}

#define SWITCH_VIC_REFUSED(stage) __atomic_store_n(&gLastRefusal, stage, __ATOMIC_RELAXED)

const char *switch_vic_last_refusal()
{
    return __atomic_load_n(&gLastRefusal, __ATOMIC_RELAXED);
}
#else
#define SWITCH_VIC_REFUSED(stage) ((void)0)
#endif
#include "../../kits/SwitchTime/SwitchTime.h"

namespace {
IOLock *gLock;
SwitchVICQueue gQueue;
TegraVICBuffer gConfigs;
uint32_t gSyncpt;
bool gSuspended, gFailed;
bool gCancelling, gRecovered, gRecoveredReady;  // a stalled job's cancellation (TegraVICCancel.cpp)
uint64_t gCompleted;
int gResults[SwitchVICQueue::kSlots];
constexpr uint32_t kThiMethod0 = 0x40;
constexpr uint64_t kQueueUs = 2000000, kMaxBoundUs = 30000000, kAcquireUs = 2000000;
constexpr uint32_t kBatchMax = 3;
static_assert(kBatchMax * (2 + 3 * TEGRA_VIC_MAX_METHODS) +
                  SWITCH_HOST1X_WAIT_WORDS * (2 + SWITCH_GPU_BRIDGE_MAX_TOKENS) <=
              SWITCH_HOST1X_ARENA_WORDS);

using Results = SwitchHost1xResults<SWITCH_GPU_ENGINE_VIC>;

int tokenStatus(const switch_gpu_backing_token *token)
{
    return Results::status(token);
}

int waitSequence(uint64_t sequence)
{
    return Results::waitLegacy(sequence);
}

int tokenWait(const switch_gpu_backing_token *token)
{
    return Results::wait(token);
}

int tokenRetain(const switch_gpu_backing_token *token)
{
    return Results::retain(token);
}

void tokenRelease(const switch_gpu_backing_token *token)
{
    Results::release(token);
}

uint32_t bound(const TegraVICJob &job, bool acquire)
{
    const uint64_t us = kQueueUs + job.pixels / 8;
    return static_cast<uint32_t>((us < kMaxBoundUs ? us : kMaxBoundUs) + (acquire ? kAcquireUs : 0));
}

struct Retirement;
using Stages = SwitchHost1xStages<Retirement, SwitchVICQueue::kSlots>;

struct Completed {
    SwitchVICQueue::Completion completion;
    void *context;
    int result;
};

// The cancelled jobs have all ended: the queue goes on from the syncpoint the engine's restart left, or refuses work
// when the engine did not come back.
void renumberLocked()
{
    const bool resumed = gRecoveredReady && gQueue.rebase(tegra_host1x_syncpt_read(gSyncpt));
    __atomic_store_n(&gFailed, !resumed, __ATOMIC_RELEASE);
    __atomic_store_n(&gCompleted, gQueue.completed, __ATOMIC_RELEASE);
    gRecovered = false;
    __atomic_store_n(&gCancelling, false, __ATOMIC_RELEASE);
    IOLockWakeup(gLock, &gCancelling, false);
    IOLockWakeup(gLock, &gQueue.count, false);
}

// The batch's last job carries its bridge, outcome, semantic batch and completion; the jobs before it retire with it.
struct Retirement {
    // Later fences of the same stall fail into the cancellation the first one started.
    static void fault(uint32_t slot, int status, int producers)
    {
        const auto &job = gQueue.jobs[slot];
        if (!__atomic_exchange_n(&gCancelling, true, __ATOMIC_ACQ_REL))
            switch_vic_job_stalled(job.fence, status, switch_now_us() - job.submittedUs, producers);
    }

    // Nothing would ever end a job whose fence or bridge could not be armed.
    [[noreturn]] static void unarmed(uint32_t slot, int status)
    {
        const auto &job = gQueue.jobs[slot];
        char engine[256], host[200];
        tegra_vic_describe(engine, sizeof(engine));
        tegra_host1x_describe_channel(TEGRA_HOST1X_CHANNEL_VIC, host, sizeof(host));
        panic("TegraVIC: fence %llu could not be armed (%d) after %llu us; %s; %s", job.fence, status,
              switch_now_us() - job.submittedUs, engine, host);
    }

    static void physical(uint32_t slot, int result)
    {
        auto &job = gQueue.jobs[slot];
        switch_gpu_bridge_release(&job.bridge);
        Results::physical(job.outcome, result);
        switch_vic_semantics_physical(0);
    }

    static void ended(uint32_t slot, int result)
    {
        IOLockLock(gLock);
        auto *semantics = gQueue.jobs[slot].semantics;
        auto *outcome = gQueue.jobs[slot].outcome;
        gQueue.jobs[slot].semantics = nullptr;
        gQueue.jobs[slot].outcome = nullptr;
        IOLockUnlock(gLock);
        if (semantics) {
            const int semantic = switch_gpu_event_waits_cached_status(semantics);
            if (semantic < 0)
                result = semantic;
            switch_gpu_event_waits_release(&semantics);
        }
        Results::complete(&outcome, result);
        Completed completed[SwitchVICQueue::kSlots];
        uint32_t count = 0;
        IOLockLock(gLock);
        Stages::stages[slot].ended = true;
        gResults[slot] = result;
        while (gQueue.count) {
            const auto &head = gQueue.jobs[gQueue.head];
            const uint32_t end = gQueue.batchEnd();
            if (!Stages::stages[end].ended)
                break;
            if (head.completion)
                completed[count++] = {head.completion, head.context, gResults[end]};
            gQueue.retire(static_cast<uint32_t>(head.fence));
        }
        if (gRecovered && !gQueue.count)
            renumberLocked();
        if (!gQueue.count)
            switch_vic_power_idle();
        __atomic_store_n(&gCompleted, gQueue.completed, __ATOMIC_RELEASE);
        IOLockWakeup(gLock, &gQueue.count, false);
        IOLockWakeup(gLock, &gQueue.completed, false);
        IOLockUnlock(gLock);
        switch_gpu_backing_completed();
        for (uint32_t i = 0; i < count; ++i)
            completed[i].completion(completed[i].context, completed[i].result < 0 ? completed[i].result : 0);
    }
};

void semanticRetired(switch_gpu_event_waits *batch)
{
    uint32_t slot = SwitchVICQueue::kSlots;
    IOLockLock(gLock);
    for (uint32_t i = 0; i < gQueue.count && slot == SwitchVICQueue::kSlots; ++i) {
        const uint32_t at = (gQueue.head + i) % SwitchVICQueue::kSlots;
        if (gQueue.jobs[at].semantics == batch)
            slot = at;
    }
    IOLockUnlock(gLock);
    if (slot < SwitchVICQueue::kSlots)
        Stages::semantic(slot);
}
}  // namespace

bool switch_vic_jobs_start(const TegraVICBuffer &configs, uint32_t syncpt, uint32_t completed)
{
    if (configs.size < kSwitchVICConfigBytes || !(gLock = IOLockAlloc()))
        return false;
    gConfigs = configs;
    gSyncpt = syncpt;
    gQueue.reset(completed);
    __atomic_store_n(&gCompleted, completed, __ATOMIC_RELEASE);
    const switch_gpu_backing_engine operations = {tokenStatus, tokenWait, tokenRetain, tokenRelease, 64};
    if (switch_gpu_backing_register_engine(SWITCH_GPU_ENGINE_VIC, &operations) ||
        !switch_vic_semantics_start(syncpt, semanticRetired))
        return false;
    (void)switch_gpu_backing_register_filter_status(SWITCH_GPU_ENGINE_VIC, switch_vic_token_filter_status);
    switch_vic_semantics_physical(completed);
    return true;
}

int switch_vic_submit_batch(const TegraVICJob *jobs, uint32_t count, const TegraVICAcquire *acquire,
                            switch_gpu_backing_use *uses, uint32_t useCount, void (*completion)(void *, int),
                            void *context, uint32_t *fence)
{
    return switch_vic_submit_owned(jobs, count, acquire, uses, useCount, nullptr, 0, completion, context, fence);
}

int switch_vic_submit_owned(const TegraVICJob *jobs, uint32_t count, const TegraVICAcquire *acquire,
                            switch_gpu_backing_use *uses, uint32_t useCount, switch_gpu_cpu_lease *const *leases,
                            uint32_t leaseCount, void (*completion)(void *, int), void *context, uint32_t *fence)
{
    const SwitchVICOwnership ownership = {uses, useCount, nullptr, 0, leases, leaseCount};
    return switch_vic_submit_checked(jobs, count, acquire, ownership, completion, context, fence);
}

int switch_vic_submit_checked(const TegraVICJob *jobs, uint32_t count, const TegraVICAcquire *acquire,
                              const SwitchVICOwnership &ownership, void (*completion)(void *, int), void *context,
                              uint32_t *fence, SwitchVICEvents *events, switch_gpu_backing_token *written)
{
    auto *uses = ownership.uses;
    uint32_t useCount = ownership.useCount;
    auto *const *leases = ownership.leases;
    const uint32_t leaseCount = ownership.leaseCount;
    if (!gLock)
        return -19;
    if (!jobs || !count || count > kBatchMax || (useCount && !uses) ||
        (ownership.guardCount && (!ownership.guards || !useCount)) || (leaseCount && (!leases || !useCount)))
        return -22;
    // A job's output is a recorded write: it moves the allocation's host epoch, so the GPU's next reader invalidates its
    // L2 before reading what VIC wrote.
    bool writes = false;
    for (uint32_t i = 0; i < useCount; ++i)
        writes |= (uses[i].access & SWITCH_GPU_BACKING_WRITE) != 0;
    if (!writes)
        return -22;
    for (uint32_t j = 0; j < count; ++j) {
        if (jobs[j].count > TEGRA_VIC_MAX_METHODS)
            return -22;
        uint32_t configs = 0;
        for (uint32_t i = 0; i < jobs[j].count; ++i)
            configs += jobs[j].method[i] == TEGRA_VIC_M_SET_CONFIG_STRUCT_OFFSET;
        if (configs != 1)
            return -22;
    }
    const SwitchVICPowerUse power;
    if (power.error)
        return power.error;
    SwitchVICDependencies dependencies;

    Results::Reservation outcome;

    struct Batch {
        switch_gpu_event_waits *value = nullptr;

        ~Batch()
        {
            switch_gpu_event_waits_abort(&value);
        }
    } batch;

    batch.value = switch_vic_events_batch(events);
    switch_gpu_event_waits_retain(batch.value);
    if (!batch.value)
        if (const int error = switch_gpu_event_waits_prepare(nullptr, 0, &batch.value))
            return error;
    const switch_gpu_backing_token *eventTokens = nullptr;
    const uint32_t eventCount = switch_vic_events_tokens(events, &eventTokens);
retry:
    if (const int error = outcome.renew()) {
        SWITCH_VIC_REFUSED("result record");
        return error;
    }
    IOLockLock(gLock);
    while ((gSuspended || gQueue.count + count > SwitchVICQueue::kSlots ||
            __atomic_load_n(&gCancelling, __ATOMIC_ACQUIRE)) &&
           !gFailed)
        IOLockSleep(gLock, &gQueue.count, THREAD_UNINT);
    if (gFailed || !gQueue.canPublish(count)) {
        IOLockUnlock(gLock);
        return -19;
    }
    switch_gpu_backing_transaction transaction = {};
    uint32_t needed = 0;
    switch_gpu_backing_token token = {};
    if (const int error = Results::prepare(outcome.value, gQueue.submitted + 1, gQueue.submitted + count, &token)) {
        IOLockUnlock(gLock);
        SWITCH_VIC_REFUSED("result token");
        return error;
    }
    int error = 0;
    if (useCount) {
        error = leaseCount
                    ? switch_gpu_backing_begin_conversion_checked(uses, useCount, leases, leaseCount, ownership.guards,
                                                                  ownership.guardCount, &token, &transaction)
                    : switch_gpu_backing_begin_checked(uses, useCount, ownership.guards, ownership.guardCount, &token,
                                                       &transaction);
        if (!error) {
            useCount = transaction.count;
            error = switch_gpu_backing_dependencies(&transaction, dependencies.tokens,
                                                    dependencies.capacity - eventCount, &needed);
        }
        if (error) {
            if (transaction.locked)
                switch_gpu_backing_abort(&transaction);
            IOLockUnlock(gLock);
            SWITCH_VIC_REFUSED("ownership: a CPU owner or guard refused a use, or dependencies");
            if (error != -28)
                return error;
            if (const int grown = dependencies.grow(needed + eventCount))
                return grown;
            goto retry;
        }
    }
    for (uint32_t i = 0; i < eventCount; ++i)
        dependencies.tokens[needed++] = eventTokens[i];
    bool capture = false;
    for (uint32_t i = 0; i < needed && !error; ++i) {
        const int covered = switch_gpu_event_waits_covers_token(batch.value, &dependencies.tokens[i]);
        capture |= covered == 0;
        if (covered < 0)
            error = covered;
    }
    if (error) {
        switch_gpu_backing_abort(&transaction);
        IOLockUnlock(gLock);
        SWITCH_VIC_REFUSED("event waits cover a dependency");
        return error;
    }
    if (capture) {
        error = dependencies.holdAll(needed);
        switch_gpu_backing_abort(&transaction);
        IOLockUnlock(gLock);
        if (error) {
            SWITCH_VIC_REFUSED("holding dependencies");
            return error;
        }
        switch_gpu_event_waits *inferred = nullptr;
        error = switch_gpu_event_waits_prepare_tokens(dependencies.tokens, needed, &inferred);
        dependencies.releaseTokens();
        if (!error && !batch.value)
            error = switch_gpu_event_waits_prepare(nullptr, 0, &batch.value);
        if (!error && inferred)
            error = switch_gpu_event_waits_merge_uncommitted(batch.value, inferred);
        switch_gpu_event_waits_abort(&inferred);
        if (error) {
            SWITCH_VIC_REFUSED("event waits for dependencies");
            return error;
        }
        goto retry;
    }
    // writer promises on what the jobs read: acquired by the GPU-to-host1x bridge, never a shared event's captured
    // dependency
    if (useCount) {
        error = switch_gpu_backing_promise_dependencies(&transaction, dependencies.tokens, dependencies.capacity,
                                                        &needed, 0, 0);
        if (error) {
            switch_gpu_backing_abort(&transaction);
            IOLockUnlock(gLock);
            SWITCH_VIC_REFUSED("writer promise dependencies");
            if (error != -28)
                return error;
            if (const int grown = dependencies.grow(needed))
                return grown;
            goto retry;
        }
    }
    bool bridgeRequired = false;
    for (uint32_t i = 0; i < needed; ++i) {
        auto &dependency = dependencies.tokens[i];
        const int ready = switch_gpu_backing_token_external_status(&dependency);
        if (ready == 1) {
            dependency = {};
            continue;
        }
        if (!ready && (dependency.engine == SWITCH_GPU_ENGINE_VIC || dependency.engine == SWITCH_GPU_ENGINE_NVDEC ||
                       dependency.engine == SWITCH_GPU_ENGINE_NVENC))
            continue;
        if (!ready &&
            (dependency.engine == SWITCH_GPU_ENGINE_GM20B || dependency.engine == SWITCH_GPU_ENGINE_PROMISE)) {
            const int covered = dependencies.covered(dependency);
            if (covered >= 0) {
                bridgeRequired |= covered == 0;
                continue;
            }
            error = covered;
        } else {
            error = ready < 0 ? ready : -95;
        }
        switch_gpu_backing_abort(&transaction);
        IOLockUnlock(gLock);
        SWITCH_VIC_REFUSED("a dependency's engine state");
        return error;
    }
    if (bridgeRequired) {
        error = dependencies.holdGpu(needed);
        switch_gpu_backing_abort(&transaction);
        IOLockUnlock(gLock);
        if (error) {
            SWITCH_VIC_REFUSED("holding GPU dependencies");
            return error;
        }
        if (const int submitted = dependencies.submitBridge()) {
            SWITCH_VIC_REFUSED("GPU bridge submission");
            return submitted;
        }
        goto retry;
    }
    uint32_t capacity = SWITCH_HOST1X_WAIT_WORDS * (needed + 2);
    for (uint32_t j = 0; j < count; ++j)
        capacity += 2 + 3 * jobs[j].count;
    uint32_t *words = nullptr;
    if (const int reserved = switch_host1x_job_reserve(TEGRA_HOST1X_CHANNEL_VIC, capacity, &words)) {
        switch_gpu_backing_abort(&transaction);
        IOLockUnlock(gLock);
        if (reserved == TEGRA_HOST1X_EAGAIN)
            error = switch_host1x_job_wait_room(TEGRA_HOST1X_CHANNEL_VIC, capacity);
        if (reserved != TEGRA_HOST1X_EAGAIN || error) {
            SWITCH_VIC_REFUSED("command arena room");
            return reserved != TEGRA_HOST1X_EAGAIN ? reserved : error;
        }
        goto retry;
    }
    SwitchVICSemantic *semantic = nullptr;
    error = switch_vic_semantics_bind(batch.value, &token, &semantic);
    if (!error)
        error = switch_vic_events_bind(events, &token);
    if (error) {
        switch_host1x_job_cancel(TEGRA_HOST1X_CHANNEL_VIC);
        switch_gpu_backing_abort(&transaction);
        IOLockUnlock(gLock);
        SWITCH_VIC_REFUSED("semantic or event binding");
        return error;
    }
    uint32_t n = 0, bridgeSyncpoint = 0, bridgeThreshold = 0;
    if (dependencies.acquire(&bridgeSyncpoint, &bridgeThreshold))
        n += tegra_host1x_op_wait_syncpt(words + n, bridgeSyncpoint, bridgeThreshold);
    if (acquire)
        n += tegra_host1x_op_wait_syncpt(words + n, acquire->syncpt, acquire->threshold);
    SwitchHost1xProducers producers;
    for (uint32_t i = 0; i < needed; ++i) {
        if (dependencies.tokens[i].engine == SWITCH_GPU_ENGINE_NVDEC ||
            dependencies.tokens[i].engine == SWITCH_GPU_ENGINE_NVENC) {
            n += tegra_host1x_op_wait_syncpt(words + n, dependencies.tokens[i].queue,
                                             static_cast<uint32_t>(dependencies.tokens[i].value));
            producers.add(dependencies.tokens[i]);
        }
    }
    for (uint32_t j = 0; j < count; ++j) {
        const auto &job = jobs[j];
        const uint64_t offset = static_cast<uint64_t>(gQueue.configurationSlot(j)) * kSwitchVICConfigStride;
        memcpy(gConfigs.va + offset, job.config, TEGRA_VIC_CONFIG_BYTES);
        switch_dcache_clean(gConfigs.va + offset, TEGRA_VIC_CONFIG_BYTES);
        words[n++] = tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_VIC, 0, 0);
        for (uint32_t i = 0; i < job.count; ++i) {
            words[n++] = tegra_host1x_op_incr(kThiMethod0 / 4, 2);
            words[n++] = job.method[i] / 4;
            words[n++] = job.method[i] == TEGRA_VIC_M_SET_CONFIG_STRUCT_OFFSET
                             ? static_cast<uint32_t>((gConfigs.pa + offset) >> 8)
                             : job.value[i];
        }
        words[n++] = tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, gSyncpt);
    }
    const uint64_t started = switch_now_us();
    const int status = switch_host1x_job_submit(TEGRA_HOST1X_CHANNEL_VIC, words, n, gSyncpt,
                                                static_cast<uint32_t>(gQueue.submitted + count));
    uint32_t end = 0;
    const uint32_t ahead = gQueue.count ? gQueue.last() : SwitchVICQueue::kSlots;
    const switch_gpu_bridge *bridge = nullptr;
    if (!status) {
        uint32_t boundUs = 0;
        for (uint32_t j = 0; j < count; ++j) {
            boundUs += bound(jobs[j], acquire != nullptr || needed != 0);
            gQueue.publish({0, started, j + 1 == count ? completion : nullptr, context,
                            j + 1 == count ? dependencies.takeBridge() : nullptr,
                            j + 1 == count ? batch.value : nullptr, j + 1 == count ? outcome.take() : nullptr,
                            count - 1 - j});
        }
        if (batch.value)
            switch_gpu_event_waits_retain(batch.value);
        if (useCount)
            switch_gpu_backing_commit(&transaction);
        switch_vic_semantics_commit(semantic);
        switch_gpu_event_waits_commit(&batch.value);
        switch_vic_events_commit(events);
        if (fence)
            *fence = static_cast<uint32_t>(gQueue.submitted);
        if (written)
            *written = token;
        end = gQueue.last();
        const auto &last = gQueue.jobs[end];
        bridge = last.bridge;
        if (const int error = Stages::start(end, gSyncpt, static_cast<uint32_t>(last.fence), bridge != nullptr,
                                            last.semantics != nullptr, boundUs, producers, ahead))
            Retirement::unarmed(end, error);
        IOLockWakeup(gLock, &gQueue.count, false);
    } else if (useCount) {
        switch_gpu_backing_abort(&transaction);
    }
    IOLockUnlock(gLock);
    if (bridge)
        if (const int error = Stages::notify(end, bridge))
            Retirement::unarmed(end, error);
    return status;
}

int switch_vic_submit_checked(const TegraVICJob *jobs, uint32_t count, const TegraVICAcquire *acquire,
                              const SwitchVICOwnership &ownership, void (*completion)(void *, int), void *context,
                              uint32_t *fence, SwitchVICEvents *events)
{
    return switch_vic_submit_checked(jobs, count, acquire, ownership, completion, context, fence, events, nullptr);
}

// switch_gpu_backing_token_filter_status for VIC: the job's own stage words, no lock. Its slot stays the job's until
// the work-loop pass that expires this fence retires it, which runs after the filter.
extern "C" int switch_vic_token_filter_status(const switch_gpu_backing_token *token)
{
    if (!token || token->engine != SWITCH_GPU_ENGINE_VIC || token->queue != gSyncpt)
        return 0;
    for (uint32_t slot = 0; slot < SwitchVICQueue::kSlots; ++slot) {
        const auto &job = gQueue.jobs[slot];
        if (__atomic_load_n(&job.fence, __ATOMIC_ACQUIRE) != token->value)
            continue;
        const auto *semantics = __atomic_load_n(&job.semantics, __ATOMIC_ACQUIRE);
        return Stages::fenceReached(slot) && (!semantics || switch_gpu_event_waits_settled_status(semantics) == 1) ? 1
                                                                                                                   : 0;
    }
    return 0;
}

extern "C" int switch_vic_wait(uint32_t fence)
{
    if (!gLock)
        return -19;
    uint64_t sequence = 0;
    IOLockLock(gLock);
    const bool valid = gQueue.legacySequence(fence, &sequence);
    IOLockUnlock(gLock);
    return valid ? waitSequence(sequence) : -22;
}

bool switch_vic_jobs_park(bool drain)
{
    IOLockLock(gLock);
    const bool parked = drain || (!gQueue.count && !__atomic_load_n(&gCancelling, __ATOMIC_ACQUIRE));
    if (parked)
        gSuspended = true;
    while (parked && gQueue.count)
        IOLockSleep(gLock, &gQueue.count, THREAD_UNINT);
    IOLockUnlock(gLock);
    return parked;
}

void switch_vic_jobs_resume(bool ready, uint32_t completed)
{
    IOLockLock(gLock);
    const bool resumed = ready && gQueue.rebase(completed);
    __atomic_store_n(&gFailed, !resumed, __ATOMIC_RELEASE);
    __atomic_store_n(&gCompleted, gQueue.completed, __ATOMIC_RELEASE);
    gSuspended = false;
    IOLockWakeup(gLock, &gQueue.count, false);
    IOLockWakeup(gLock, &gQueue.completed, false);
    IOLockUnlock(gLock);
    switch_vic_semantics_physical(gQueue.completed);
    switch_gpu_backing_completed();
}

uint32_t switch_vic_jobs_cancel(uint32_t reached)
{
    IOLockLock(gLock);
    for (uint32_t i = 0; i < gQueue.count; ++i) {
        const uint32_t slot = (gQueue.head + i) % SwitchVICQueue::kSlots;
        if (!gQueue.jobs[slot].toBatchEnd && Stages::cancel(slot, reached))
            Results::physical(gQueue.jobs[slot].outcome, Stages::kCancelled);
    }
    const auto through = static_cast<uint32_t>(gQueue.submitted);
    IOLockUnlock(gLock);
    return through;
}

void switch_vic_jobs_recovered(bool ready)
{
    IOLockLock(gLock);
    gRecovered = true;
    gRecoveredReady = ready;
    const bool drained = !gQueue.count;
    if (drained)
        renumberLocked();
    IOLockUnlock(gLock);
    if (drained)
        switch_vic_power_idle();
}

void switch_vic_jobs_settle()
{
    IOLockLock(gLock);
    while (__atomic_load_n(&gCancelling, __ATOMIC_ACQUIRE))
        IOLockSleep(gLock, &gCancelling, THREAD_UNINT);
    IOLockUnlock(gLock);
}
