// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchPadctl.h"
#include "../SwitchMMIO/SwitchMMIOKernel.h"

namespace {
IOLock *gLock;

// The first user allocates it: PCIe, the XUDC and the host come up in no fixed order.
IOLock *padctlLock()
{
    IOLock *lock = __atomic_load_n(&gLock, __ATOMIC_ACQUIRE);
    if (lock)
        return lock;
    IOLock *fresh = IOLockAlloc();
    if (!fresh)
        panic("SwitchPadctl: no memory for the pad controller lock");
    IOLock *none = nullptr;
    if (__atomic_compare_exchange_n(&gLock, &none, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return fresh;
    IOLockFree(fresh);
    return none;
}
}  // namespace

extern "C" IOLock *switch_padctl_lock(void)
{
    IOLock *lock = padctlLock();
    IOLockLock(lock);
    return lock;
}

extern "C" void switch_padctl_unlock(void)
{
    IOLockUnlock(__atomic_load_n(&gLock, __ATOMIC_ACQUIRE));
}

extern "C" void switch_padctl_modify(volatile uint8_t *padctl, uint32_t offset, uint32_t clear, uint32_t set)
{
    const SwitchPadctlLocked locked;
    switch_mmio_rmw32(padctl, offset, clear, set);
}
