// SPDX-License-Identifier: BSD-2-Clause-Patent
// The XUSB pad controller's lock (XUSB_PADCTL at 0x7009F000; Linux padctl->lock). USB device and host mode, their
// SuperSpeed lane and PCIe's UPHY bring-up (drivers/SwitchWLAN) each read-modify-write pad controller registers that
// another of them also writes: SS_PORT_MAP, USB2_PAD_MUX, USB2_PORT_CAP, USB2_VBUS_ID, ELPG_PROGRAM_0/1, USB3_PAD_MUX
// and USB2 pad 0's OTG, charger and bias controls. Every such write sequence, in any driver, runs under this lock.
// Thread context only; it may be held across the pads' microsecond delays.
#ifndef SWITCH_PADCTL_H
#define SWITCH_PADCTL_H

#include <IOKit/IOLib.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns the lock, now held.
IOLock *switch_padctl_lock(void);
void switch_padctl_unlock(void);
// One read-modify-write under the lock, on the caller's mapping of the pad controller.
void switch_padctl_modify(volatile uint8_t *padctl, uint32_t offset, uint32_t clear, uint32_t set);

#ifdef __cplusplus
}

class SwitchPadctlLocked {
public:
    SwitchPadctlLocked() : lock(switch_padctl_lock())
    {}

    ~SwitchPadctlLocked()
    {
        IOLockUnlock(lock);
    }

    SwitchPadctlLocked(const SwitchPadctlLocked &) = delete;
    SwitchPadctlLocked &operator=(const SwitchPadctlLocked &) = delete;
    SwitchPadctlLocked(SwitchPadctlLocked &&) = delete;
    SwitchPadctlLocked &operator=(SwitchPadctlLocked &&) = delete;

private:
    IOLock *lock;
};
#endif

#endif
