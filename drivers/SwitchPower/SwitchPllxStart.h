// SPDX-License-Identifier: BSD-2-Clause-Patent
// PLLX started again at new dividers in the order the Tegra X1 TRM (DP-07225-001 v1.3p) gives in 5.1.5 "To start the
// PLL": out of IDDQ, 5 us, the registers while IDDQ and ENABLE are 0, ENABLE 0 to 1, then the lock. PLLX_MISC
// LOCK_ENABLE resets to 0 (5.2.42, software default 1) and is set before ENABLE so PLLX_LOCK (5.2.41) reports the lock;
// PLLX_MISC_3 IDDQ resets to 1 (5.2.197). Kernel-free: SwitchThermal.cpp runs it on the CAR with the cluster parked off
// PLLX, test/switch_pllx_start_test.cpp on a recording model.
#ifndef SWITCH_PLLX_START_H
#define SWITCH_PLLX_START_H

#include <stdint.h>

namespace switchpllx {
constexpr uint32_t kBase = 0x0e0, kMisc = 0x0e4, kMisc3 = 0x518;
constexpr uint32_t kBypass = 1u << 31, kEnable = 1u << 30, kLock = 1u << 27, kLockEnable = 1u << 18, kIddq = 1u << 3;

// `regs` has read(offset) and write(offset, value) on the CAR, `delayUs(us)` busy-waits, `waitLock()` waits out the
// lock time. Returns PLLX_BASE as read after that wait; the caller checks kLock.
template <class Regs, class Delay, class WaitLock>
uint32_t start(Regs &regs, uint32_t base, Delay delayUs, WaitLock waitLock)
{
    regs.write(kBase, regs.read(kBase) & ~kEnable);
    if (regs.read(kMisc3) & kIddq) {
        regs.write(kMisc3, regs.read(kMisc3) & ~kIddq);
        (void)regs.read(kMisc3);
        delayUs(5);
    }
    const uint32_t stopped = base & ~(kEnable | kBypass | kLock);
    regs.write(kBase, stopped);
    regs.write(kMisc, regs.read(kMisc) | kLockEnable);
    regs.write(kBase, stopped | kEnable);
    (void)regs.read(kBase);
    waitLock();
    return regs.read(kBase);
}
}  // namespace switchpllx

#endif
