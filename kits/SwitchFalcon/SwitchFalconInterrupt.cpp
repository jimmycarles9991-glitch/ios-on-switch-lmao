// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchFalconInterrupt.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <kern/thread_call.h>
#include "../../drivers/ArmGIC/SwitchInterrupts.h"

bool SwitchFalconInterrupt::start(const char *name, uint32_t intid, volatile uint8_t *engine, Failed failed)
{
    name_ = name;
    intid_ = intid;
    engine_ = engine;
    failed_ = failed;
    call_ = thread_call_allocate_with_options(&deferredAction, this, THREAD_CALL_PRIORITY_KERNEL, 0);
    IOService *nub = call_ ? switch_interrupt_nub(intid, false) : nullptr;
    if (!nub || nub->registerInterrupt(0, nullptr, &interruptHandler, this) != kIOReturnSuccess) {
        if (call_) {
            thread_call_free(call_);
            call_ = nullptr;
        }
        IOLog("%s: falcon error interrupt (INTID %u) could not be attached\n", name, intid);
        return false;
    }
    nub_ = nub;
    return true;
}

void SwitchFalconInterrupt::on()
{
    if (!nub_)
        return;
    onAt_ = taken();
    __atomic_store_n(&powered_, true, __ATOMIC_RELEASE);
#if defined(__aarch64__) && !defined(SWITCH_MMIO_HOST_MODEL)
    __asm__ volatile("dmb ishst" ::: "memory");
#endif
    switch_mmio_write32(engine_, SWITCH_FALCON_THI_INT_MASK, SWITCH_FALCON_THI_FALCON_INT);
    nub_->enableInterrupt(0);
}

void SwitchFalconInterrupt::off()
{
    if (!nub_)
        return;
    // disableInterrupt synchronizes with any active primary handler on another CPU before clearing powered_.
    nub_->disableInterrupt(0);
    __atomic_store_n(&powered_, false, __ATOMIC_RELEASE);
}

void SwitchFalconInterrupt::interruptHandler(OSObject *, void *refCon, IOService *nub, int source)
{
    auto *self = static_cast<SwitchFalconInterrupt *>(refCon);
    if (!__atomic_load_n(&self->powered_, __ATOMIC_ACQUIRE))
        return;
    if (!switch_falcon_error_read(self->engine_, &self->error_))
        return;
    __atomic_store_n(&self->taken_, self->taken_ + 1, __ATOMIC_RELEASE);
    // The falcon stays parked with the level line asserted; soft-disable causes ArmGIC to mask the line before EOI
    // until cancellation resets the engine and calls on().
    nub->disableInterrupt(source);
    thread_call_enter(self->call_);
}

void SwitchFalconInterrupt::deferredAction(void *param, void *)
{
    auto *self = static_cast<SwitchFalconInterrupt *>(param);
    self->failed_(self->error_);
}

void SwitchFalconInterrupt::lostCheckStart()
{
    if (!nub_)
        return;
    checkPending_ = __atomic_load_n(&powered_, __ATOMIC_ACQUIRE) && switch_falcon_error_pending(engine_);
}

void SwitchFalconInterrupt::lostCheckEnd()
{
    if (!nub_ || !checkPending_ || !switch_falcon_error_pending(engine_) || taken() != onAt_ ||
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
