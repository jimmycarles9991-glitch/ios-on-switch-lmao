// SPDX-License-Identifier: BSD-2-Clause-Patent
// See SwitchHost1xTokenFence.h. A VIC or NVDEC token is the job's syncpoint (queue) and its fence number, whose low 32
// bits are the syncpoint's threshold: the engines keep their sequences on the hardware's counter across SC7 (rebase).
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include "SwitchHost1xTokenFence.h"
#include "SwitchHost1xFence.h"
#include "TegraHost1x.h"

namespace {
constexpr uint32_t kJoins = 16;

struct Join {
    switch_host1x_fence fences[SWITCH_HOST1X_TOKEN_FENCE_MAX];
    switch_gpu_backing_token tokens[SWITCH_HOST1X_TOKEN_FENCE_MAX];
    uint32_t count, pending;
    int status;
    void (*completed)(void *, int);
    void *context;
    bool used;
};

IOLock *gLock;  // the joins; fences are added under it, so no callback reaches a join before it is whole
Join gJoins[kJoins];

IOLock *lock()
{
    if (!__atomic_load_n(&gLock, __ATOMIC_ACQUIRE)) {
        IOLock *fresh = IOLockAlloc();
        // NOLINTNEXTLINE(misc-const-correctness): the compare-exchange writes it
        IOLock *expected = nullptr;
        if (!__atomic_compare_exchange_n(&gLock, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            IOLockFree(fresh);
    }
    return gLock;
}

// The engine's own record of the job (SwitchHost1xResults): 0 pending, 1 done, or its failure. Its hardware outcome is
// read first. A cancelled job is recorded failed before the CPU releases its syncpoint, and a job that ran on a failed
// producer's memory once its own fence (queued on the syncpoint ahead of this one) is in; the job's result waits for
// its semantic outcome, which is retired on another thread after this fence.
int status(const switch_gpu_backing_token &token)
{
    if (token.engine != SWITCH_GPU_ENGINE_VIC && token.engine != SWITCH_GPU_ENGINE_NVDEC)
        return -22;
    const int physical = switch_gpu_backing_token_physical_status(&token, 0);
    return physical < 0 ? physical : switch_gpu_backing_token_status(&token);
}

void signaled(switch_host1x_fence *fence, int failure)
{
    const auto offset = reinterpret_cast<uintptr_t>(fence) - reinterpret_cast<uintptr_t>(gJoins);
    Join &join = gJoins[offset / sizeof(Join)];
    IOLockLock(gLock);
    if (failure && !join.status)
        join.status = failure;
    const bool last = !--join.pending;
    const Join done = last ? join : Join{};
    if (last)
        join.used = false;
    IOLockUnlock(gLock);
    if (!last)
        return;
    int result = done.status;
    for (uint32_t i = 0; i < done.count && !result; ++i) {
        const int reported = status(done.tokens[i]);
        if (reported < 0)
            result = reported;
    }
    done.completed(done.context, result);
}
}  // namespace

extern "C" int switch_host1x_token_fence(const switch_gpu_backing_token *tokens, uint32_t count,
                                         void (*completed)(void *, int), void *context)
{
    if (!tokens || !count || count > SWITCH_HOST1X_TOKEN_FENCE_MAX || !completed || !lock())
        return -22;
    for (uint32_t i = 0; i < count; ++i) {
        const int reported = status(tokens[i]);
        if (reported < 0)
            return reported;
    }
    IOLockLock(gLock);
    Join *join = nullptr;
    for (auto &candidate : gJoins)
        if (!candidate.used && !join)
            join = &candidate;
    if (!join) {
        IOLockUnlock(gLock);
        return -16;
    }
    join->used = true;
    join->count = join->pending = count;
    join->status = 0;
    join->completed = completed;
    join->context = context;
    for (uint32_t i = 0; i < count; ++i) {
        const auto &token = tokens[i];
        join->tokens[i] = token;
        const uint32_t threshold =
            status(token) == 1 ? tegra_host1x_syncpt_read(token.queue) : static_cast<uint32_t>(token.value);
        switch_host1x_fence_init(&join->fences[i], token.queue, threshold, signaled);
        const int error = switch_host1x_fence_add(&join->fences[i], 0);
        if (!error)
            continue;
        if (!i) {
            join->used = false;
            IOLockUnlock(gLock);
            return error;
        }
        join->status = error;
        join->pending -= count - i;
        break;
    }
    IOLockUnlock(gLock);
    return 0;
}
