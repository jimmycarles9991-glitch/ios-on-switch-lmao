// SPDX-License-Identifier: BSD-2-Clause-Patent
// The fence lists in the kernel: one lock, the syncpoint interrupt's action, the work-loop hand-off and one timer for
// the bounds; and the filter waiters, which the interrupt's primary filter runs under a simple lock. See
// SwitchHost1xFence.h.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread_call.h>
#include "SwitchHost1xFence.h"
#include "SwitchHost1xFenceList.h"
#include "SwitchHost1xIntr.h"
#include "TegraHost1x.h"
#include "../ArmGIC/SwitchInterrupts.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"

namespace {
constexpr uint32_t kGraceMs = 50;

struct Registers {
    volatile uint8_t *sync;

    uint32_t read(uint32_t offset) const
    {
        return switch_mmio_read32(sync, offset);
    }

    void write(uint32_t offset, uint32_t value) const
    {
        switch_mmio_write32(sync, offset, value);
    }
};

IOSimpleLock *gLock;  // protects fence lists, filter waiters, hand-off, and bound timer across IRQ filter & threads
Registers gRegisters;
SwitchHost1xFenceList<Registers> gList;
// Syncpoints whose channel stopped (switch_host1x_fences_fail) and has not restarted: a bound or a CPU wait that starts
// on one fails at once, as the stop would have failed it.
uint32_t gStopped[SwitchHost1xFenceList<Registers>::kSyncpts / 32];

bool stoppedLocked(uint32_t id)
{
    return (gStopped[id / 32] & (1u << (id % 32))) != 0;
}

SwitchHost1xExpired gHandOff;
IOInterruptEventSource *gKick;
thread_call_t gBounds;
uint64_t gBoundsAt;
uint32_t gIntid;
bool gUp, gStrict;

// A filter waiter's own fence (first, so the list's callback finds the waiter) arms its threshold and is expired by
// the pass like any other. Free -> waiting (published once queued) -> retired (cancelled while down) -> free.
struct FilterWaiter {
    switch_host1x_fence fence;
    void (*fn)(void *);
    void *ctx;
    uint32_t generation;
    uint8_t state;
    bool fired;
};

enum : uint8_t {
    kWaiterFree,
    kWaiterRetired,
    kWaiterWaiting
};

constexpr uint32_t kFilterWaiters = 8;
FilterWaiter gWaiters[kFilterWaiters];

void waiterExpired(switch_host1x_fence *, int)
{}

uint64_t deadlineIn(uint32_t us)
{
    uint64_t deadline = 0;
    clock_interval_to_deadline(us, kMicrosecondScale, &deadline);
    return deadline;
}

void armBoundsLocked(uint64_t deadline)
{
    if (gBoundsAt && gBoundsAt <= deadline)
        return;
    gBoundsAt = deadline;
    thread_call_enter_delayed(gBounds, deadline);
}

void cancelBoundsLocked(bool force = false)
{
    if (!gBoundsAt || (!force && gList.hasBounded()))
        return;
    gBoundsAt = 0;
    thread_call_cancel(gBounds);
}

// Waiters (no callback) and filter waiters are retired here under gLock (filter-safe); callbacks move to `callbacks`.
void sortLocked(SwitchHost1xExpired &done, SwitchHost1xExpired &callbacks, uint8_t state)
{
    for (auto *fence = done.take(); fence;) {
        auto *next = fence->next;
        if (fence->signaled == waiterExpired) {
            reinterpret_cast<FilterWaiter *>(fence)->state = kWaiterFree;
            fence->state = SWITCH_HOST1X_FENCE_IDLE;
        } else if (fence->signaled) {
            fence->state = state;
            callbacks.push(fence);
        } else {
            fence->state = SWITCH_HOST1X_FENCE_IDLE;
            thread_wakeup(reinterpret_cast<event_t>(fence));
        }
        fence = next;
    }
}

// Outside the work loop, callbacks are handed to it; returns true when caller must kick gKick after unlocking gLock.
bool settleLocked(SwitchHost1xExpired &done)
{
    cancelBoundsLocked();
    const bool idle = !gHandOff.first;
    sortLocked(done, gHandOff, SWITCH_HOST1X_FENCE_HANDED_OFF);
    return idle && gHandOff.first != nullptr;
}

void maybeKick(bool kick)
{
    if (kick)
        gKick->interruptOccurred(nullptr, nullptr, 0);
}

[[noreturn]] void panicLost(const switch_host1x_fence *fence)
{
    char gic[192];
    switch_interrupt_describe(gIntid, gic, sizeof(gic));
    const uint32_t id = fence->syncpt;
    panic("TegraHost1x: syncpoint %u threshold interrupt lost (INTID %u): value %u threshold %u INT_STATUS[%u] %#x, %s",
          id, gIntid, gList.value(gRegisters, id), fence->threshold, id / 32, gList.status(gRegisters, id), gic);
}

void kicked(OSObject *, IOInterruptEventSource *, int)
{
    switch_host1x_intr_handle();
}

void bounds(thread_call_param_t, thread_call_param_t)
{
    SwitchHost1xExpired done;
    uint64_t grace = 0, next = 0;
    nanoseconds_to_absolutetime(static_cast<uint64_t>(kGraceMs) * NSEC_PER_MSEC, &grace);
    const IOInterruptState ints = IOSimpleLockLockDisableInterrupt(gLock);
    gBoundsAt = 0;
    const switch_host1x_fence *lost = nullptr;
    if (gUp && gList.hasBounded()) {
        lost = gList.overdue(gRegisters, mach_absolute_time(), grace, gStrict, TEGRA_HOST1X_ETIMEDOUT,
                             TEGRA_HOST1X_ELOST, done, &next);
        if (!lost && next)
            armBoundsLocked(next);
    }
    const bool kick = settleLocked(done);
    IOSimpleLockUnlockEnableInterrupt(gLock, ints);
    if (lost)
        panicLost(lost);
    maybeKick(kick);
}
}  // namespace

bool switch_host1x_intr_init(IOWorkLoop *loop, volatile uint8_t *sync, uint32_t intid)
{
    gRegisters.sync = sync;
    gIntid = intid;
    gBounds = thread_call_allocate_with_options(bounds, nullptr, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    gKick = IOInterruptEventSource::interruptEventSource(loop, &kicked);
    IOSimpleLock *lock = IOSimpleLockAlloc();
    if (!lock || !gBounds || !gKick || loop->addEventSource(gKick) != kIOReturnSuccess)
        return false;
    gKick->enable();
    __atomic_store_n(&gLock, lock, __ATOMIC_RELEASE);
    return true;
}

bool switch_host1x_intr_filter()
{
    IOSimpleLock *lock = __atomic_load_n(&gLock, __ATOMIC_ACQUIRE);
    if (!lock)
        return false;
    IOSimpleLockLock(lock);
    bool needAction = false;
    if (gUp) {
        for (auto &waiter : gWaiters) {
            if (waiter.state != kWaiterWaiting || waiter.fired ||
                !switch_host1x_reached(gList.value(gRegisters, waiter.fence.syncpt), waiter.fence.threshold))
                continue;
            waiter.fired = true;
            waiter.fn(waiter.ctx);
        }
        SwitchHost1xExpired done;
        const bool fired = gList.interrupt(gRegisters, done);
        cancelBoundsLocked();
        sortLocked(done, gHandOff, SWITCH_HOST1X_FENCE_HANDED_OFF);
        if (fired)
            (void)gList.status(gRegisters, 0);  // flush posted INT_DISABLE/INT_STATUS before dropping level SPI
        needAction = gHandOff.first != nullptr;
    }
    IOSimpleLockUnlock(lock);
    return needAction;
}

void switch_host1x_intr_handle()
{
    SwitchHost1xExpired callbacks, done;
    const IOInterruptState ints = IOSimpleLockLockDisableInterrupt(gLock);
    done.append(gHandOff);
    if (gUp) {
        gList.interrupt(gRegisters, done);
        cancelBoundsLocked();
    }
    sortLocked(done, callbacks, SWITCH_HOST1X_FENCE_DELIVERING);
    IOSimpleLockUnlockEnableInterrupt(gLock, ints);
    for (auto *fence = callbacks.take(); fence;) {
        auto *next = fence->next;
        auto *signaled = fence->signaled;
        const int status = fence->status;
        __atomic_store_n(&fence->state, SWITCH_HOST1X_FENCE_IDLE, __ATOMIC_RELEASE);
        signaled(fence, status);
        fence = next;
    }
}

uint32_t switch_host1x_fences_fail(uint32_t id, int status)
{
    if (!gLock || id >= SwitchHost1xFenceList<Registers>::kSyncpts)
        return 0;
    SwitchHost1xExpired done;
    uint32_t failed = 0;
    const IOInterruptState ints = IOSimpleLockLockDisableInterrupt(gLock);
    gStopped[id / 32] |= 1u << (id % 32);
    if (gUp)
        failed = gList.fail(gRegisters, id, status, done);
    const bool kick = settleLocked(done);
    IOSimpleLockUnlockEnableInterrupt(gLock, ints);
    maybeKick(kick);
    return failed;
}

void switch_host1x_fences_resume(uint32_t id)
{
    if (!gLock || id >= SwitchHost1xFenceList<Registers>::kSyncpts)
        return;
    const IOInterruptState ints = IOSimpleLockLockDisableInterrupt(gLock);
    gStopped[id / 32] &= ~(1u << (id % 32));
    IOSimpleLockUnlockEnableInterrupt(gLock, ints);
}

bool switch_host1x_fence_names(uint32_t id)
{
    if (!gLock || id >= SwitchHost1xFenceList<Registers>::kSyncpts)
        return false;
    const IOInterruptState ints = IOSimpleLockLockDisableInterrupt(gLock);
    const bool named = gList.names(id);
    IOSimpleLockUnlockEnableInterrupt(gLock, ints);
    return named;
}

void switch_host1x_intr_stop()
{
    const IOInterruptState ints = IOSimpleLockLockDisableInterrupt(gLock);
    gUp = false;
    cancelBoundsLocked(true);
    IOSimpleLockUnlockEnableInterrupt(gLock, ints);
}

void switch_host1x_intr_start()
{
    SwitchHost1xExpired done;
    const IOInterruptState ints = IOSimpleLockLockDisableInterrupt(gLock);
    gUp = true;
    gList.rearm(gRegisters, done);
    const bool kick = settleLocked(done);
    IOSimpleLockUnlockEnableInterrupt(gLock, ints);
    maybeKick(kick);
}

extern "C" void switch_host1x_fence_init(switch_host1x_fence *fence, uint32_t syncpt, uint32_t threshold,
                                         void (*signaled)(switch_host1x_fence *, int))
{
    *fence = {};
    fence->syncpt = syncpt;
    fence->threshold = threshold;
    fence->signaled = signaled;
}

extern "C" int switch_host1x_fence_add(switch_host1x_fence *fence, uint32_t boundUs)
{
    if (!gLock)
        return TEGRA_HOST1X_ENODEV;
    if (!fence || fence->syncpt >= SwitchHost1xFenceList<Registers>::kSyncpts ||
        __atomic_load_n(&fence->state, __ATOMIC_ACQUIRE) != SWITCH_HOST1X_FENCE_IDLE)
        return TEGRA_HOST1X_EINVAL;
    SwitchHost1xExpired done;
    const IOInterruptState ints = IOSimpleLockLockDisableInterrupt(gLock);
    if (!gUp) {
        IOSimpleLockUnlockEnableInterrupt(gLock, ints);
        return TEGRA_HOST1X_ENODEV;
    }
    fence->graced = 0;
    fence->deadline = boundUs ? deadlineIn(boundUs) : 0;
    gList.add(gRegisters, fence, done);
    if (fence->state == SWITCH_HOST1X_FENCE_QUEUED && fence->deadline && stoppedLocked(fence->syncpt))
        gList.fail(gRegisters, fence->syncpt, TEGRA_HOST1X_ETIMEDOUT, done);
    if (fence->state == SWITCH_HOST1X_FENCE_QUEUED && fence->deadline)
        armBoundsLocked(fence->deadline);
    const bool kick = settleLocked(done);
    IOSimpleLockUnlockEnableInterrupt(gLock, ints);
    maybeKick(kick);
    return TEGRA_HOST1X_OK;
}

extern "C" void switch_host1x_fence_watch(switch_host1x_fence *fence, uint32_t boundUs)
{
    if (!gLock || !fence)
        return;
    SwitchHost1xExpired done;
    const IOInterruptState ints = IOSimpleLockLockDisableInterrupt(gLock);
    if (fence->state == SWITCH_HOST1X_FENCE_QUEUED) {
        fence->graced = 0;
        gList.setDeadline(fence, deadlineIn(boundUs));
        if (stoppedLocked(fence->syncpt))
            gList.fail(gRegisters, fence->syncpt, TEGRA_HOST1X_ETIMEDOUT, done);
        else
            armBoundsLocked(fence->deadline);
    }
    const bool kick = settleLocked(done);
    IOSimpleLockUnlockEnableInterrupt(gLock, ints);
    maybeKick(kick);
}

extern "C" bool switch_host1x_fence_remove(switch_host1x_fence *fence)
{
    if (!gLock || !fence)
        return false;
    SwitchHost1xExpired done;
    const IOInterruptState ints = IOSimpleLockLockDisableInterrupt(gLock);
    bool removed = gUp && gList.remove(gRegisters, fence, done);
    if (!removed && fence->state == SWITCH_HOST1X_FENCE_HANDED_OFF && gHandOff.erase(fence)) {
        fence->state = SWITCH_HOST1X_FENCE_IDLE;
        removed = true;
    }
    const bool kick = settleLocked(done);
    IOSimpleLockUnlockEnableInterrupt(gLock, ints);
    maybeKick(kick);
    return removed;
}

extern "C" int switch_host1x_filter_wait(uint32_t syncpt, uint32_t threshold, void (*fn)(void *), void *ctx,
                                         switch_host1x_filter_handle *handle)
{
    if (!gLock)
        return TEGRA_HOST1X_ENODEV;
    if (syncpt >= SwitchHost1xFenceList<Registers>::kSyncpts || !fn || !handle)
        return TEGRA_HOST1X_EINVAL;
    const IOInterruptState ints = IOSimpleLockLockDisableInterrupt(gLock);
    if (!gUp) {
        IOSimpleLockUnlockEnableInterrupt(gLock, ints);
        return TEGRA_HOST1X_ENODEV;
    }
    if (switch_host1x_reached(gList.value(gRegisters, syncpt), threshold)) {
        IOSimpleLockUnlockEnableInterrupt(gLock, ints);
        return SWITCH_HOST1X_FILTER_PASSED;
    }
    FilterWaiter *waiter = nullptr;
    for (auto &slot : gWaiters)
        if (!waiter && slot.state == kWaiterFree)
            waiter = &slot;
    if (!waiter) {
        IOSimpleLockUnlockEnableInterrupt(gLock, ints);
        return TEGRA_HOST1X_ENOMEM;
    }
    waiter->state = kWaiterRetired;
    switch_host1x_fence_init(&waiter->fence, syncpt, threshold, waiterExpired);
    waiter->fn = fn;
    waiter->ctx = ctx;
    waiter->fired = false;
    ++waiter->generation;
    SwitchHost1xExpired done;
    gList.add(gRegisters, &waiter->fence, done);
    const bool queued = waiter->fence.state == SWITCH_HOST1X_FENCE_QUEUED;
    if (queued)
        waiter->state = kWaiterWaiting;
    const bool kick = settleLocked(done);
    *handle = (uint64_t(waiter->generation) << 32) | uint64_t(waiter - gWaiters);
    IOSimpleLockUnlockEnableInterrupt(gLock, ints);
    maybeKick(kick);
    return queued ? int(TEGRA_HOST1X_OK) : int(SWITCH_HOST1X_FILTER_PASSED);
}

// While host1x is down its list cannot be touched: the waiter is only retired, and its fence expires after the resume.
extern "C" bool switch_host1x_filter_cancel(switch_host1x_filter_handle handle)
{
    const uint64_t index = handle & 0xffffffffu;
    if (!gLock || index >= kFilterWaiters)
        return false;
    auto &waiter = gWaiters[index];
    const IOInterruptState ints = IOSimpleLockLockDisableInterrupt(gLock);
    const bool mine = waiter.generation == uint32_t(handle >> 32) && waiter.state == kWaiterWaiting;
    const bool ran = waiter.fired;
    if (mine)
        waiter.state = kWaiterRetired;
    SwitchHost1xExpired done;
    if (mine && gUp && gList.remove(gRegisters, &waiter.fence, done))
        waiter.state = kWaiterFree;
    const bool kick = settleLocked(done);
    IOSimpleLockUnlockEnableInterrupt(gLock, ints);
    maybeKick(kick);
    return mine && !ran;
}

// A waiter is a fence without a callback, woken under the lock. A wait that ends with the syncpoint past the threshold
// but its fence still queued, after a grace period for an action under way, means the interrupt never reached the CPU:
// a panic once strict (an engine was proven and published), TEGRA_HOST1X_ELOST while its probe is still running.
extern "C" int tegra_host1x_syncpt_wait(uint32_t id, uint32_t threshold, uint32_t timeoutUs)
{
    return tegra_host1x_syncpt_wait_unless(id, threshold, timeoutUs, nullptr, 0);
}

extern "C" void tegra_host1x_syncpt_kick(uint32_t id)
{
    if (!gLock || id >= SwitchHost1xFenceList<Registers>::kSyncpts)
        return;
    const IOInterruptState ints = IOSimpleLockLockDisableInterrupt(gLock);
    gList.forEach(id, [](switch_host1x_fence *fence) {
        if (!fence->signaled)
            thread_wakeup(reinterpret_cast<event_t>(fence));
    });
    IOSimpleLockUnlockEnableInterrupt(gLock, ints);
}

extern "C" int tegra_host1x_syncpt_wait_unless(uint32_t id, uint32_t threshold, uint32_t timeoutUs,
                                               const uint32_t *errors, uint32_t seen)
{
    if (!gLock)
        return TEGRA_HOST1X_ENODEV;
    if (id >= SwitchHost1xFenceList<Registers>::kSyncpts)
        return TEGRA_HOST1X_EINVAL;
    switch_host1x_fence waiter;
    switch_host1x_fence_init(&waiter, id, threshold, nullptr);
    SwitchHost1xExpired done;
    IOInterruptState ints = IOSimpleLockLockDisableInterrupt(gLock);
    if (!gUp) {
        IOSimpleLockUnlockEnableInterrupt(gLock, ints);
        return TEGRA_HOST1X_ENODEV;
    }
    gList.add(gRegisters, &waiter, done);
    if (waiter.state == SWITCH_HOST1X_FENCE_QUEUED && stoppedLocked(id))
        gList.fail(gRegisters, id, TEGRA_HOST1X_ETIMEDOUT, done);
    bool kick = settleLocked(done);
    uint64_t deadline = deadlineIn(timeoutUs);
    int ret = TEGRA_HOST1X_OK;
    bool lostPanic = false;
    for (bool grace = false; waiter.state == SWITCH_HOST1X_FENCE_QUEUED;) {
        if (errors && __atomic_load_n(errors, __ATOMIC_ACQUIRE) != seen) {
            ret = TEGRA_HOST1X_EFALCON;
            break;
        }
        if (kick) {
            IOSimpleLockUnlockEnableInterrupt(gLock, ints);
            maybeKick(true);
            kick = false;
            ints = IOSimpleLockLockDisableInterrupt(gLock);
            continue;
        }
        wait_result_t wr = assert_wait_deadline(reinterpret_cast<event_t>(&waiter), THREAD_UNINT, deadline);
        if (wr == THREAD_WAITING) {
            IOSimpleLockUnlockEnableInterrupt(gLock, ints);
            wr = thread_block(THREAD_CONTINUE_NULL);
            ints = IOSimpleLockLockDisableInterrupt(gLock);
        }
        if (wr != THREAD_TIMED_OUT)
            continue;
        if (waiter.state != SWITCH_HOST1X_FENCE_QUEUED)
            break;
        if (!switch_host1x_reached(gList.value(gRegisters, id), threshold)) {
            ret = TEGRA_HOST1X_ETIMEDOUT;  // the engine did not get there: its failure, not the interrupt's
            break;
        }
        if (grace) {
            if (!__atomic_load_n(&gStrict, __ATOMIC_ACQUIRE)) {
                ret = TEGRA_HOST1X_ELOST;
                break;
            }
            lostPanic = true;
            break;
        }
        grace = true;
        clock_interval_to_deadline(kGraceMs, kMillisecondScale, &deadline);
    }
    if (!lostPanic) {
        gList.remove(gRegisters, &waiter, done);
        kick |= settleLocked(done);
    }
    IOSimpleLockUnlockEnableInterrupt(gLock, ints);
    if (lostPanic)
        panicLost(&waiter);
    maybeKick(kick);
    return ret ? ret : waiter.status;
}

extern "C" void tegra_host1x_set_strict(void)
{
    __atomic_store_n(&gStrict, true, __ATOMIC_RELEASE);
}
