// SPDX-License-Identifier: BSD-2-Clause-Patent
// Window A's armed completion flip written from its producer's primary interrupt filter (SwitchCompletionFlipDisplay.h):
// the GPU's nonstall filter calls gpuFilterFlip for a watched present whose producer is complete and visible, host1x's
// syncpoint filter calls vicFilterFlip for a VIC copy's job. Both run with interrupts off under their producer's simple
// lock and take only completionFlipLock. Included in the class body after SwitchCompletionFlipDisplay.h.

// A VIC copy's host1x filter waiter, one per arm parity: a waiter outlives its offer only by the work-loop pass that
// retires it, so the next arm never shares its slot.
struct VicFilterWaiter {
    void *display;
    uint32_t generation;
    switch_gpu_backing_token producer;
};

static inline VicFilterWaiter vicFilterWaiters[2] = {};

template <class Self> static void gpuFilterFlip(void *display, int status)
{
    if (!status)
        static_cast<Self>(display)->writeEarlyCompletionFlip(SwitchCompletionFlip::kAnyArm,
                                                             SwitchCompletionFlip::kFromGpuFilter);
}

template <class Self> static void vicFilterFlip(void *ctx)
{
    const auto *waiter = static_cast<const VicFilterWaiter *>(ctx);
    if (switch_gpu_backing_token_filter_status(&waiter->producer) == 1)
        static_cast<Self>(waiter->display)
            ->writeEarlyCompletionFlip(waiter->generation, SwitchCompletionFlip::kFromVicFilter);
}

// The copy thread, the VIC job's fence armed: host1x's syncpoint filter writes the armed flip once the job is done.
// A job already done, or a waiter refused, leaves it to the fence's completion.
void armCompletionFlipWaiter(const switch_gpu_backing_token &producer)
{
    IOSimpleLock *lock = __atomic_load_n(&completionFlipLock, __ATOMIC_ACQUIRE);
    if (!lock || __atomic_load_n(&completionFilterOff, __ATOMIC_RELAXED))
        return;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    const bool armed = completionFlip.state == SwitchCompletionFlip::kArmed;
    const uint32_t generation = completionFlip.generation;
    IOSimpleLockUnlockEnableInterrupt(lock, state);
    if (!armed)
        return;
    auto &waiter = vicFilterWaiters[generation & 1];
    waiter = {this, generation, producer};
    switch_host1x_filter_handle handle = 0;
    const int waiting = switch_host1x_filter_wait(producer.queue, static_cast<uint32_t>(producer.value),
                                                  &vicFilterFlip<decltype(this)>, &waiter, &handle);
    if (waiting && SWITCH_FRAME_TIMELINE)
        noteFlipTotal(7, 1);
}

// The producer's primary interrupt filter (interrupts off, the producer's own simple lock held): no allocation, no
// log, nothing but this lock.
void writeEarlyCompletionFlip(uint32_t armed, uint8_t by)
{
    IOSimpleLock *lock = __atomic_load_n(&completionFlipLock, __ATOMIC_ACQUIRE);
    if (!lock || __atomic_load_n(&completionFilterOff, __ATOMIC_RELAXED))
        return;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    const bool write = !gPanicRestore.panicking && completionFlip.claimEarly(armed, by);
    if (write)
        writeCompletionFlipLocked();
    [[maybe_unused]] const uint32_t start = completionFlip.start;
    IOSimpleLockUnlockEnableInterrupt(lock, state);
    if (write && SWITCH_FRAME_TIMELINE) {
        noteFlipTotal(5, 1);
        KDBG(SWITCH_KDBG_CODE(SWITCH_KDBG_FLIP), start);
    }
}

// gpuComplete's first step: a flip its producer's filter already wrote is on screen, so its completion is a success
// (the filter wrote only for a job certain to end so). A failure here breaks that: one line, and no filter writes
// from then on.
int completionFlipVerdict(int status)
{
    status = mediaFlipVerdict(status);
    if (!status || !__atomic_load_n(&completionFlipLock, __ATOMIC_ACQUIRE))
        return status;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(completionFlipLock);
    const bool early = completionFlip.early();
    const int entry = completionFlip.entry;
    IOSimpleLockUnlockEnableInterrupt(completionFlipLock, state);
    if (!early)
        return status;
    if (!__atomic_exchange_n(&completionFilterOff, true, __ATOMIC_RELAXED))
        IOLog("SwitchDisplay: window A was flipped to entry %d from its producer's interrupt filter, whose completion "
              "then failed (%d): shown as done; producers' filters write no flips from here\n",
              entry, status);
    return 0;
}
