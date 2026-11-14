// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchFalconInterrupt.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include "../../drivers/ArmGIC/SwitchInterrupts.h"

namespace {
constexpr uint32_t kEngines = 4;
SwitchFalconInterrupt *gEngines[kEngines];
IOFilterInterruptEventSource *gSources[kEngines];
uint32_t gCount;
IOWorkLoop *gLoop;
IOLock *gRegistering;

IOLock *registering()
{
    if (!__atomic_load_n(&gRegistering, __ATOMIC_ACQUIRE)) {
        IOLock *fresh = IOLockAlloc();
        // NOLINTNEXTLINE(misc-const-correctness): the compare-exchange writes it
        IOLock *expected = nullptr;
        if (fresh &&
            !__atomic_compare_exchange_n(&gRegistering, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            IOLockFree(fresh);
    }
    return __atomic_load_n(&gRegistering, __ATOMIC_ACQUIRE);
}

SwitchFalconInterrupt *engineOf(IOInterruptEventSource *source)
{
    for (uint32_t i = 0; i < __atomic_load_n(&gCount, __ATOMIC_ACQUIRE); i++)
        if (gSources[i] == source)
            return gEngines[i];
    return nullptr;
}

// NOLINTNEXTLINE(misc-const-correctness): IOFilterInterruptEventSource::Filter's signature
bool filtered(OSObject *, IOFilterInterruptEventSource *source)
{
    SwitchFalconInterrupt *engine = engineOf(source);
    return engine && engine->filter();
}

void occurred(OSObject *, IOInterruptEventSource *source, int)
{
    if (SwitchFalconInterrupt *engine = engineOf(source))
        engine->action();
}
}  // namespace

// Every engine registers from its own bring-up thread; the work loop and the table slot are made once, in order.
bool SwitchFalconInterrupt::start(const char *name, uint32_t intid, volatile uint8_t *engine, Failed failed)
{
    IOLock *lock = registering();
    name_ = name;
    intid_ = intid;
    engine_ = engine;
    failed_ = failed;
    lock_ = IOSimpleLockAlloc();
    if (!lock || !lock_) {
        IOLog("%s: falcon error interrupt (INTID %u) could not be attached: no lock\n", name, intid);
        return false;
    }
    IOLockLock(lock);
    if (!gLoop)
        gLoop = IOWorkLoop::workLoop();
    const uint32_t slot = gCount;
    IOService *nub = switch_interrupt_nub(intid, false);
    source_ = gLoop && nub && slot < kEngines
                  ? IOFilterInterruptEventSource::filterInterruptEventSource(gLoop, &occurred, &filtered, nub, 0)
                  : nullptr;
    if (source_) {
        gEngines[slot] = this;
        gSources[slot] = source_;
        __atomic_store_n(&gCount, slot + 1, __ATOMIC_RELEASE);
    }
    const bool added = source_ && gLoop->addEventSource(source_) == kIOReturnSuccess;
    IOLockUnlock(lock);
    if (!added)
        IOLog("%s: falcon error interrupt (INTID %u) could not be attached\n", name, intid);
    return added;
}

void SwitchFalconInterrupt::on()
{
    if (!source_)
        return;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock_);
    powered_ = true;
    onAt_ = taken_;
    switch_mmio_write32(engine_, SWITCH_FALCON_THI_INT_MASK, SWITCH_FALCON_THI_FALCON_INT);
    IOSimpleLockUnlockEnableInterrupt(lock_, state);
    source_->enable();
}

void SwitchFalconInterrupt::off()
{
    if (!source_)
        return;
    source_->disable();
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock_);
    powered_ = false;
    IOSimpleLockUnlockEnableInterrupt(lock_, state);
}

bool SwitchFalconInterrupt::filter()
{
    IOSimpleLockLock(lock_);
    const bool took = powered_ && switch_falcon_error_read(engine_, &error_);
    if (took)
        __atomic_store_n(&taken_, taken_ + 1, __ATOMIC_RELEASE);
    IOSimpleLockUnlock(lock_);
    return took;
}

// The falcon waits parked on its error with the line still asserted: the line stays off until the engine's
// cancellation has cleared it, reset the engine and booted it again (on()).
void SwitchFalconInterrupt::action()
{
    source_->disable();
    failed_(error_);
}

void SwitchFalconInterrupt::lostCheckStart()
{
    if (!source_)
        return;
    checkPending_ = __atomic_load_n(&powered_, __ATOMIC_ACQUIRE) && switch_falcon_error_pending(engine_);
}

void SwitchFalconInterrupt::lostCheckEnd()
{
    if (!source_ || !checkPending_ || !switch_falcon_error_pending(engine_) || taken() != onAt_ ||
        !__atomic_load_n(&powered_, __ATOMIC_ACQUIRE) ||
        !(switch_mmio_read32(engine_, SWITCH_FALCON_THI_INT_MASK) & SWITCH_FALCON_THI_FALCON_INT))
        return;
    char gic[192];
    char error[256];
    switch_falcon_error seen = {};
    (void)switch_falcon_error_read(engine_, &seen);
    switch_interrupt_describe(intid_, gic, sizeof(gic));
    panic("%s: falcon error interrupt lost (INTID %u): THI INT_STATUS %#x INT_MASK %#x, %s; %s", name_, intid_,
          switch_mmio_read32(engine_, SWITCH_FALCON_THI_INT_STATUS),
          switch_mmio_read32(engine_, SWITCH_FALCON_THI_INT_MASK),
          switch_falcon_error_describe(&seen, error, sizeof(error)), gic);
}
