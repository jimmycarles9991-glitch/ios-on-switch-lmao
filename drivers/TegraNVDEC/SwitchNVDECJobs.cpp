// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/thread.h>
#include <string.h>
#include "SwitchNVDECJobs.h"
#include "SwitchNVDECQueue.h"
#include "SwitchNVDECSemantics.h"
#include "../TegraHost1x/SwitchHost1xResults.h"
#include "../TegraHost1x/SwitchHost1xDependencies.h"
#include "../TegraHost1x/SwitchHost1xStages.h"
#include "SwitchNVDECAccess.h"
#include "TegraNVDEC.h"
#include "TegraNVDECCheck.h"
#if SWITCH_NVDEC_PROBES
#include "SwitchNVDECDisplayProbe.h"
#endif
#include "../TegraGM20B/SwitchGpuBacking.h"
#include "../TegraHost1x/TegraHost1x.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchTime/SwitchTime.h"

namespace {
constexpr uint32_t kSetupStride = 1024, kThiMethod0 = 0x40;
constexpr uint32_t kWaitBoundUs = SwitchNVDECQueue::kSlots * 400000;
static_assert(uint32_t(SWITCH_NVDEC_READ) == uint32_t(SWITCH_GPU_BACKING_READ) &&
              uint32_t(SWITCH_NVDEC_WRITE) == uint32_t(SWITCH_GPU_BACKING_WRITE));
IOLock *gLock;
SwitchNVDECQueue gQueue;
switch_dma_buffer gSetup;
uint32_t gSyncpt;
uint64_t gCompleted;
bool gSuspended, gFailed;

struct Submission {
    TegraNVDECBlock blocks[TEGRA_NVDEC_MAX_ENTRIES] = {};
    switch_gpu_backing_use uses[TEGRA_NVDEC_MAX_ENTRIES] = {};
    uint32_t *words = nullptr;
    uint32_t useCount = 0;
};

bool gCancelling, gRecovered, gRecoveredReady;  // a stalled job's cancellation (TegraNVDECCancel.cpp)

using Results = SwitchHost1xResults<SWITCH_GPU_ENGINE_NVDEC>;

int status(const switch_gpu_backing_token *token)
{
    return Results::status(token);
}

int waitSequence(uint64_t sequence)
{
    return Results::waitLegacy(sequence);
}

int wait(const switch_gpu_backing_token *token)
{
    return Results::wait(token);
}

int retain(const switch_gpu_backing_token *token)
{
    return Results::retain(token);
}

void release(const switch_gpu_backing_token *token)
{
    Results::release(token);
}

struct Retirement;
using Stages = SwitchHost1xStages<Retirement, SwitchNVDECQueue::kSlots>;

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

struct Retirement {
    // Later fences of the same stall fail into the cancellation the first one started.
    static void fault(uint32_t slot, int status, int)
    {
        const auto &job = gQueue.jobs[slot];
        if (!__atomic_exchange_n(&gCancelling, true, __ATOMIC_ACQ_REL))
            switch_nvdec_job_stalled(static_cast<uint32_t>(job.sequence), status, switch_now_us() - job.submittedUs);
    }

    // Nothing would ever end a job whose fence or bridge could not be armed.
    [[noreturn]] static void unarmed(uint32_t slot, int status)
    {
        const auto &job = gQueue.jobs[slot];
        switch_nvdec_job_fault(static_cast<uint32_t>(job.sequence), status, switch_now_us() - job.submittedUs);
    }

    static void physical(uint32_t slot, int result)
    {
        auto &job = gQueue.jobs[slot];
        switch_gpu_bridge_release(&job.bridge);
        Results::physical(job.outcome, result);
        switch_nvdec_semantics_physical(0);
    }

    static void ended(uint32_t slot, int result)
    {
        IOLockLock(gLock);
        auto &job = gQueue.jobs[slot];
        auto *semantics = job.semantics;
        auto *outcome = job.outcome;
        const auto completion = job.completion;
        const auto fence = static_cast<uint32_t>(job.sequence);
        job.semantics = nullptr;
        job.outcome = nullptr;
        job.completion = {};
        IOLockUnlock(gLock);
        if (semantics) {
            const int semantic = switch_gpu_event_waits_cached_status(semantics);
            if (semantic < 0)
                result = semantic;
            switch_gpu_event_waits_release(&semantics);
        }
        if (completion.done)
            completion.done(completion.context, fence, result < 0 ? result : 0);
        Results::complete(&outcome, result);
        IOLockLock(gLock);
        Stages::stages[slot].ended = true;
        while (gQueue.count && Stages::stages[gQueue.head].ended)
            gQueue.retire(static_cast<uint32_t>(gQueue.jobs[gQueue.head].sequence));
        if (gRecovered && !gQueue.count)
            renumberLocked();
        if (!gQueue.count)
            switch_nvdec_power_idle();
        __atomic_store_n(&gCompleted, gQueue.completed, __ATOMIC_RELEASE);
        IOLockWakeup(gLock, &gQueue.count, false);
        IOLockWakeup(gLock, &gQueue.completed, false);
        IOLockUnlock(gLock);
        switch_gpu_backing_completed();
    }
};

void semanticRetired(switch_gpu_event_waits *batch)
{
    uint32_t slot = SwitchNVDECQueue::kSlots;
    IOLockLock(gLock);
    for (uint32_t i = 0; i < gQueue.count && slot == SwitchNVDECQueue::kSlots; ++i) {
        const uint32_t at = (gQueue.head + i) % SwitchNVDECQueue::kSlots;
        if (gQueue.jobs[at].semantics == batch)
            slot = at;
    }
    IOLockUnlock(gLock);
    if (slot < SwitchNVDECQueue::kSlots)
        Stages::semantic(slot);
}

using Check = const char *(*)(const TegraNVDECSubmit *, const uint64_t *);

int prepare(const TegraNVDECSubmit &submit, TegraNVDECResolve resolve, void *context, Check check, Submission &work,
            const char **why)
{
    uint64_t avail[TEGRA_NVDEC_MAX_ENTRIES] = {};
    for (uint32_t i = 0; i < submit.entry_count; ++i) {
        const auto &entry = submit.entries[i];
        if (entry.kind != TEGRA_NVDEC_ENTRY_RELOC)
            continue;
        auto &block = work.blocks[i];
        if (!resolve(context, entry.value, &block) || !block.backing || entry.offset > block.size) {
            *why = "relocation outside a buffer";
            return -22;
        }
        avail[i] = block.size - entry.offset;
    }
    *why = check(&submit, avail);
    if (*why)
        return -22;
    for (uint32_t i = 0; i < submit.entry_count; ++i) {
        if (submit.entries[i].kind != TEGRA_NVDEC_ENTRY_RELOC)
            continue;
        const uint32_t access = switch_nvdec_entry_access(submit, i);
        if (!access) {
            *why = "unclassified buffer access";
            return -22;
        }
        work.uses[work.useCount++] = {work.blocks[i].backing, access, 0};
    }
    for (uint32_t i = 0; i < submit.clean_count; ++i) {
        const auto &range = submit.clean[i];
        TegraNVDECBlock block = {};
        if (!resolve(context, range.handle, &block) || !block.backing ||
            static_cast<uint64_t>(range.offset) + range.length > block.size) {
            *why = "clean range";
            return -22;
        }
        if (!range.length)
            continue;
        // The caller already wrote this range. Acquiring it must preserve those dirty CPU lines.
        constexpr uint32_t access = SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_CPU_NO_CACHE;
        int error = switch_gpu_backing_cpu_acquire(block.backing, range.offset, range.length, access);
        if (error)
            return error;
        error = switch_gpu_backing_cpu_publish(block.backing, range.offset, range.length);
        const int released = switch_gpu_backing_cpu_release(block.backing, range.offset, range.length, access);
        if (error || released)
            return error ? error : released;
    }
    return 0;
}

static_assert(SWITCH_HOST1X_WAIT_WORDS * (SWITCH_GPU_BRIDGE_MAX_TOKENS + 1) + 3 * (TEGRA_NVDEC_MAX_ENTRIES + 1) + 2 <=
              SWITCH_HOST1X_ARENA_WORDS);

int publish(const TegraNVDECSubmit &submit, Submission &work, const SwitchNVDECCompletion &completion, uint32_t *fence,
            bool *waitedForRoom)
{
    SwitchHost1xDependencies dependencies;
    Results::Reservation outcome;

    struct Batch {
        switch_gpu_event_waits *value = nullptr;

        ~Batch()
        {
            switch_gpu_event_waits_abort(&value);
        }
    } batch;

    if (const int error = switch_gpu_event_waits_prepare(nullptr, 0, &batch.value))
        return error;
    for (;;) {
        if (const int error = outcome.renew())
            return error;
        IOLockLock(gLock);
        while ((gSuspended || gQueue.full() || __atomic_load_n(&gCancelling, __ATOMIC_ACQUIRE)) && !gFailed) {
            *waitedForRoom = true;
            IOLockSleep(gLock, &gQueue.count, THREAD_UNINT);
        }
        if (gFailed || gQueue.next() == 0) {
            IOLockUnlock(gLock);
            return -19;
        }
        switch_gpu_backing_token token = {};
        if (const int error = Results::prepare(outcome.value, gQueue.next(), gQueue.next(), &token)) {
            IOLockUnlock(gLock);
            return error;
        }
        switch_gpu_backing_transaction transaction = {};
        uint32_t needed = 0;
        int error = switch_gpu_backing_begin(work.uses, work.useCount, &token, &transaction);
        if (!error) {
            work.useCount = transaction.count;
            error = switch_gpu_backing_dependencies(&transaction, dependencies.tokens, dependencies.capacity, &needed);
        }
        if (error) {
            if (transaction.locked)
                switch_gpu_backing_abort(&transaction);
            IOLockUnlock(gLock);
            if (error != -28)
                return error;
            if (const int grown = dependencies.grow(needed))
                return grown;
            continue;
        }
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
            return error;
        }
        if (capture) {
            error = dependencies.holdAll(needed);
            switch_gpu_backing_abort(&transaction);
            IOLockUnlock(gLock);
            if (error)
                return error;
            switch_gpu_event_waits *inferred = nullptr;
            error = switch_gpu_event_waits_prepare_tokens(dependencies.tokens, needed, &inferred);
            dependencies.releaseTokens();
            if (!error && !batch.value)
                error = switch_gpu_event_waits_prepare(nullptr, 0, &batch.value);
            if (!error && inferred)
                error = switch_gpu_event_waits_merge_uncommitted(batch.value, inferred);
            switch_gpu_event_waits_abort(&inferred);
            if (error)
                return error;
            continue;
        }
        // writer promises on what the job reads: acquired by the GPU-to-host1x bridge, never a shared event's captured
        // dependency
        error = switch_gpu_backing_promise_dependencies(&transaction, dependencies.tokens, dependencies.capacity,
                                                        &needed, 0, 0);
        if (error) {
            switch_gpu_backing_abort(&transaction);
            IOLockUnlock(gLock);
            if (error != -28)
                return error;
            if (const int grown = dependencies.grow(needed))
                return grown;
            continue;
        }
        bool bridgeRequired = false;
        for (uint32_t i = 0; i < needed && !error; ++i) {
            auto &dependency = dependencies.tokens[i];
            const int ready = switch_gpu_backing_token_external_status(&dependency);
            if (ready == 1) {
                dependency = {};
                continue;
            }
            if (!ready && (dependency.engine == SWITCH_GPU_ENGINE_NVDEC || dependency.engine == SWITCH_GPU_ENGINE_VIC ||
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
            } else
                error = ready < 0 ? ready : -95;
        }
        if (error) {
            switch_gpu_backing_abort(&transaction);
            IOLockUnlock(gLock);
            return error;
        }
        if (bridgeRequired) {
            error = dependencies.holdGpu(needed);
            switch_gpu_backing_abort(&transaction);
            IOLockUnlock(gLock);
            if (error)
                return error;
            if (const int submitted = dependencies.submitBridge())
                return submitted;
            continue;
        }
        const uint32_t capacity = SWITCH_HOST1X_WAIT_WORDS * (needed + 1) + 3 * (submit.entry_count + 1) + 2;
        if (const int reserved = switch_host1x_job_reserve(TEGRA_HOST1X_CHANNEL_DEFAULT, capacity, &work.words)) {
            switch_gpu_backing_abort(&transaction);
            IOLockUnlock(gLock);
            if (reserved != TEGRA_HOST1X_EAGAIN)
                return reserved;
            if (const int waited = switch_host1x_job_wait_room(TEGRA_HOST1X_CHANNEL_DEFAULT, capacity))
                return waited;
            continue;
        }
        SwitchNVDECSemantic *semantic = nullptr;
        error = switch_nvdec_semantics_bind(batch.value, &token, &semantic);
        if (error) {
            switch_host1x_job_cancel(TEGRA_HOST1X_CHANNEL_DEFAULT);
            switch_gpu_backing_abort(&transaction);
            IOLockUnlock(gLock);
            return error;
        }
        uint32_t n = 0, bridgeSyncpoint = 0, bridgeThreshold = 0;
        if (dependencies.acquire(&bridgeSyncpoint, &bridgeThreshold))
            n += tegra_host1x_op_wait_syncpt(work.words + n, bridgeSyncpoint, bridgeThreshold);
        SwitchHost1xProducers producers;
        for (uint32_t i = 0; i < needed; ++i)
            if (dependencies.tokens[i].engine == SWITCH_GPU_ENGINE_VIC ||
                dependencies.tokens[i].engine == SWITCH_GPU_ENGINE_NVENC) {
                n += tegra_host1x_op_wait_syncpt(work.words + n, dependencies.tokens[i].queue,
                                                 static_cast<uint32_t>(dependencies.tokens[i].value));
                producers.add(dependencies.tokens[i]);
            }
        const uint64_t slot = static_cast<uint64_t>(gQueue.tail()) * kSetupStride;
        memcpy(gSetup.va + slot, submit.setup, submit.setup_bytes);
        switch_dcache_clean(gSetup.va + slot, submit.setup_bytes);
        work.words[n++] = tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_NVDEC, 0, 0);
        const auto method = [&](uint32_t offset, uint32_t value) {
            work.words[n++] = tegra_host1x_op_incr(kThiMethod0 / 4, 2);
            work.words[n++] = offset / 4;
            work.words[n++] = value;
        };
        for (uint32_t i = 0; i < submit.entry_count; ++i) {
            const auto &entry = submit.entries[i];
            if (i + 1 == submit.entry_count)
                method(NVC5B0_SET_DRV_PIC_SETUP_OFFSET, static_cast<uint32_t>((gSetup.pa + slot) >> 8));
            method(entry.method, entry.kind == TEGRA_NVDEC_ENTRY_RELOC
                                     ? static_cast<uint32_t>((work.blocks[i].iova + entry.offset) >> 8)
                                     : entry.value);
        }
        work.words[n++] = tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, gSyncpt);
        const uint64_t began = switch_now_us();
        error = switch_host1x_job_submit(TEGRA_HOST1X_CHANNEL_DEFAULT, work.words, n, gSyncpt,
                                         static_cast<uint32_t>(gQueue.next()));
        const uint32_t queued = gQueue.tail();
        const uint32_t ahead = gQueue.count ? (queued + SwitchNVDECQueue::kSlots - 1) % SwitchNVDECQueue::kSlots
                                            : SwitchNVDECQueue::kSlots;
        const switch_gpu_bridge *bridge = nullptr;
        if (!error) {
            gQueue.publish(began, dependencies.takeBridge(), batch.value, outcome.take(), completion);
            if (batch.value)
                switch_gpu_event_waits_retain(batch.value);
            switch_gpu_backing_commit(&transaction);
            switch_nvdec_semantics_commit(semantic);
            switch_gpu_event_waits_commit(&batch.value);
            *fence = static_cast<uint32_t>(gQueue.submitted);
            const auto &job = gQueue.jobs[queued];
            bridge = job.bridge;
            if (const int failed = Stages::start(queued, gSyncpt, *fence, bridge != nullptr, job.semantics != nullptr,
                                                 kWaitBoundUs, producers, ahead))
                Retirement::unarmed(queued, failed);
            IOLockWakeup(gLock, &gQueue.count, false);
        } else {
            switch_gpu_backing_abort(&transaction);
        }
        IOLockUnlock(gLock);
        if (bridge)
            if (const int failed = Stages::notify(queued, bridge))
                Retirement::unarmed(queued, failed);
        return error;
    }
}
}  // namespace

bool switch_nvdec_jobs_start(const switch_dma_buffer &setup, uint32_t syncpt, uint32_t completed)
{
    if (setup.size < SwitchNVDECQueue::kSlots * kSetupStride || !(gLock = IOLockAlloc()))
        return false;
    gSetup = setup;
    gSyncpt = syncpt;
    gQueue.submitted = gQueue.completed = completed;
    __atomic_store_n(&gCompleted, completed, __ATOMIC_RELEASE);
    const switch_gpu_backing_engine operations = {status, wait, retain, release, 64};
    if (switch_gpu_backing_register_engine(SWITCH_GPU_ENGINE_NVDEC, &operations) ||
        !switch_nvdec_semantics_start(syncpt, semanticRetired)) {
        IOLockFree(gLock);
        gLock = nullptr;
        return false;
    }
    (void)switch_gpu_backing_register_filter_status(SWITCH_GPU_ENGINE_NVDEC, switch_nvdec_token_filter_status);
    switch_nvdec_semantics_physical(completed);
    return true;
}

namespace {
int run(const TegraNVDECSubmit *submit, TegraNVDECResolve resolve, void *context, Check check,
        const SwitchNVDECCompletion *completion, uint32_t *fence, bool *waitedForRoom, const char **why)
{
    if (!why || !waitedForRoom)
        return -22;
    *why = nullptr;
    *waitedForRoom = false;
    if (!gLock || !tegra_nvdec_ready())
        return -19;
    if (!submit || !resolve || !fence || submit->entry_count > TEGRA_NVDEC_MAX_ENTRIES ||
        submit->clean_count > TEGRA_NVDEC_MAX_RANGES || submit->setup_bytes > kSetupStride) {
        *why = "counts";
        return -22;
    }
    auto *work = IONewZero(Submission, 1);
    if (!work)
        return -12;
    int error = prepare(*submit, resolve, context, check, *work, why);
    if (!error)
        error = switch_nvdec_power_use();
    if (!error) {
        error = publish(*submit, *work, completion ? *completion : SwitchNVDECCompletion{}, fence, waitedForRoom);
        switch_nvdec_power_done();
    }
    IODelete(work, Submission, 1);
    return error;
}

#if SWITCH_NVDEC_PROBES
const char *displayProbeCheck(const TegraNVDECSubmit *submit, const uint64_t *avail)
{
    struct Scratch {
        TegraNVDECSubmit submit;
        uint64_t avail[TEGRA_NVDEC_MAX_ENTRIES];
    };

    auto *scratch = IONew(Scratch, 1);
    if (!scratch)
        return "probe: no memory for the check";
    const char *why = switch_nvdec_check_display_probe(submit, avail, &scratch->submit, scratch->avail);
    IODelete(scratch, Scratch, 1);
    return why;
}
#endif
}  // namespace

extern "C" int tegra_nvdec_run(const TegraNVDECSubmit *submit, TegraNVDECResolve resolve, void *context,
                               const SwitchNVDECCompletion *completion, uint32_t *fence, bool *waitedForRoom,
                               const char **why)
{
    return run(submit, resolve, context, tegra_nvdec_check, completion, fence, waitedForRoom, why);
}

#if SWITCH_NVDEC_PROBES
extern "C" int switch_nvdec_run_display_probe(const TegraNVDECSubmit *submit, TegraNVDECResolve resolve, void *context,
                                              const SwitchNVDECCompletion *completion, uint32_t *fence,
                                              bool *waitedForRoom, const char **why)
{
    return run(submit, resolve, context, displayProbeCheck, completion, fence, waitedForRoom, why);
}
#endif

// switch_gpu_backing_token_filter_status for NVDEC: the job's own stage words, no lock. Its slot stays the job's until
// the work-loop pass that expires this fence retires it, which runs after the filter.
extern "C" int switch_nvdec_token_filter_status(const switch_gpu_backing_token *token)
{
    if (!token || token->engine != SWITCH_GPU_ENGINE_NVDEC || token->queue != gSyncpt)
        return 0;
    for (uint32_t slot = 0; slot < SwitchNVDECQueue::kSlots; ++slot) {
        const auto &job = gQueue.jobs[slot];
        if (__atomic_load_n(&job.sequence, __ATOMIC_ACQUIRE) != token->value)
            continue;
        const auto *semantics = __atomic_load_n(&job.semantics, __ATOMIC_ACQUIRE);
        return Stages::fenceReached(slot) && (!semantics || switch_gpu_event_waits_settled_status(semantics) == 1) ? 1
                                                                                                                   : 0;
    }
    return 0;
}

extern "C" int tegra_nvdec_wait(uint32_t fence)
{
    if (!gLock)
        return -19;
    uint64_t sequence = 0;
    IOLockLock(gLock);
    const bool valid = gQueue.legacySequence(fence, &sequence);
    IOLockUnlock(gLock);
    return valid ? waitSequence(sequence) : -22;
}

bool switch_nvdec_jobs_park(bool drain)
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

void switch_nvdec_jobs_resume(bool ready, uint32_t completed)
{
    IOLockLock(gLock);
    if (!gQueue.rebase(completed))
        panic("SwitchNVDEC: syncpoint reset lost its completed timeline");
    __atomic_store_n(&gCompleted, gQueue.completed, __ATOMIC_RELEASE);
    switch_nvdec_semantics_physical(gQueue.completed);
    __atomic_store_n(&gFailed, !ready, __ATOMIC_RELEASE);
    gSuspended = false;
    IOLockWakeup(gLock, &gQueue.count, false);
    IOLockWakeup(gLock, &gQueue.completed, false);
    IOLockUnlock(gLock);
    switch_gpu_backing_completed();
}

uint32_t switch_nvdec_jobs_cancel(uint32_t reached)
{
    IOLockLock(gLock);
    for (uint32_t i = 0; i < gQueue.count; ++i) {
        const uint32_t slot = (gQueue.head + i) % SwitchNVDECQueue::kSlots;
        if (Stages::cancel(slot, reached))
            Results::physical(gQueue.jobs[slot].outcome, Stages::kCancelled);
    }
    const auto through = static_cast<uint32_t>(gQueue.submitted);
    IOLockUnlock(gLock);
    return through;
}

void switch_nvdec_jobs_recovered(bool ready)
{
    IOLockLock(gLock);
    gRecovered = true;
    gRecoveredReady = ready;
    const bool drained = !gQueue.count;
    if (drained)
        renumberLocked();
    IOLockUnlock(gLock);
    if (drained)
        switch_nvdec_power_idle();
}

void switch_nvdec_jobs_settle()
{
    IOLockLock(gLock);
    while (__atomic_load_n(&gCancelling, __ATOMIC_ACQUIRE))
        IOLockSleep(gLock, &gCancelling, THREAD_UNINT);
    IOLockUnlock(gLock);
}
