// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchEMC's hints and the hold's end (included by SwitchEMC.cpp). A hint comes from any context: the touch filter, the
// frame core's start edge in primary interrupt context, a missed frame on the display's gate, exec and the GPU and
// host1x submit threads on any CPU. Each writes the deadline slot of the CPU it runs on with interrupts masked, so every
// slot has one writer and no hint needs a read-modify-write; the work loop takes the latest when it decides. The slots
// are times and publish nothing else: the loop reads them after interruptOccurred's work-loop lock.

uint64_t SwitchEMC::hintUntil() const
{
    uint64_t until = 0;
    for (const HintSlot &slot : hints) {
        const uint64_t at = __atomic_load_n(&slot.until, __ATOMIC_RELAXED);
        if (at > until)
            until = at;
    }
    return until;
}

// The top rate at once, kept for the hold.
void SwitchEMC::hint()
{
    const boolean_t enabled = ml_set_interrupts_enabled(FALSE);
    const auto cpu = static_cast<unsigned>(cpu_number());
    if (cpu < switchperf::kSignalCpus) {
        HintSlot &slot = hints[cpu];
        const uint64_t now = mach_absolute_time();
        __atomic_store_n(&slot.until, now + holdTicks, __ATOMIC_RELAXED);
        if (!__atomic_load_n(&gTopHeld, __ATOMIC_ACQUIRE)) {
#if DEVELOPMENT || DEBUG
            if (!__atomic_load_n(&slot.asked, __ATOMIC_RELAXED))
                __atomic_store_n(&slot.asked, now, __ATOMIC_RELAXED);
#endif
            signal[cpu]->interruptOccurred(nullptr, nullptr, 0);
        }
    }
    ml_set_interrupts_enabled(enabled);
}

#if DEVELOPMENT || DEBUG
// The loop: the first of the hints that asked for this raise, their slots cleared. A slot's CPU stores a time only while
// the slot reads 0, and the loop clears only a slot it read as a time, so a CPU's store always follows the clear it saw.
uint64_t SwitchEMC::takeAsked()
{
    uint64_t first = 0;
    for (HintSlot &slot : hints) {
        const uint64_t at = __atomic_load_n(&slot.asked, __ATOMIC_RELAXED);
        if (!at)
            continue;
        __atomic_store_n(&slot.asked, 0, __ATOMIC_RELAXED);
        if (!first || at < first)
            first = at;
    }
    return first;
}
#endif

// The CPU governor's tick (interrupts masked, one CPU at a time): the loop woken once when the hold has ended, instead of
// a hold timer chasing the last frame.
void SwitchEMC::tick(uint64_t now)
{
    if (!switch_emc_hold_tick(&holdState, now, hintUntil(), switch_idle_last_frame()))
        return;
    const auto cpu = static_cast<unsigned>(cpu_number());
    if (cpu < switchperf::kSignalCpus)
        signal[cpu]->interruptOccurred(nullptr, nullptr, 0);
}

IOReturn SwitchEMC::retickAction(OSObject *owner, void *, void *, void *, void *)
{
    auto *self = static_cast<SwitchEMC *>(owner);
    if (self->governing)
        self->program();
    return kIOReturnSuccess;
}

// The CPU governor started or stopped (thread context): the hold is programmed again, its end watched by the tick or by
// the hold timer.
void SwitchEMC::retick()
{
    loop->runAction(&SwitchEMC::retickAction, this);
}
