// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchXHCI's class. SwitchXHCI.cpp is the controller (start, the register overrides, the bring-up),
// SwitchXHCIEvents.cpp its events (the falcon mailbox, the lost-interrupt check, the role, the USB-C cable, SC7).
#pragma once
#include "OriginalAppleUSBXHCI.h"
#include "UsbDmaMapper.h"
#include "XusbHost.h"
#include "XusbPorts.h"
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>

extern "C" {
// tegra210b01_xusb_firmware (firmware.S, drivers/SwitchUSB/firmware/README.md).
extern const uint8_t switch_xusb_firmware[] __asm__("_switch_xusb_firmware");
extern const uint8_t switch_xusb_firmware_end[] __asm__("_switch_xusb_firmware_end");
}

namespace xhost {
extern Controller gHost;  // SwitchXHCI.cpp
constexpr uint32_t kRequiredInterrupters = 3;

inline size_t firmwareBytes()
{
    return reinterpret_cast<uintptr_t>(switch_xusb_firmware_end) - reinterpret_cast<uintptr_t>(switch_xusb_firmware);
}
}  // namespace xhost

class SwitchXHCI : public AppleUSBXHCI {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchXHCI);

    IOWorkLoop *tegraLoop;  // mailbox and the lost-interrupt check; AppleUSBXHCI keeps its own loop
    IOFilterInterruptEventSource *mailbox;
    IOTimerEventSource *check;
    volatile uint32_t mailboxInterrupts;
    uint32_t unclaimed;  // primary interrupt context only
    volatile bool mailboxStorm;
    xusb_irq_sample hostAtCheck, mailboxAtCheck;
    uint8_t checkStage;
    bool firstCheck, checked, gaveUp;
    // Interrupter fold (SwitchXHCI.cpp).
    uint32_t hardwareInterrupters, runtimeOffset, operationalOffset;
    uint64_t capBase;  // AppleUSBXHCI's mapping of the capability registers
    uint32_t shadow[xhost::kRequiredInterrupters][8];

    static bool mailboxFilter(OSObject *owner, IOFilterInterruptEventSource *);
    static void mailboxAction(OSObject *owner, IOInterruptEventSource *, int);
    void armCheck(uint32_t ms, uint8_t stage);
    void checkFired(IOTimerEventSource *);
    static xusb_irq_sample hostSample();
    xusb_irq_sample mailboxSample() const;
    uint32_t *shadowFor(uint64_t address);

public:
    static SwitchXHCI *running;

    IOWorkLoop *tegraLoopForCheck() const
    {
        return tegraLoop;
    }

    IOReturn expectPortChange();
    IOReturn systemSleep(bool entering);
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    uint32_t regRead32(uint64_t base, uint32_t offset) override;
    uint64_t regRead64(uint64_t base, uint32_t offset) override;
    void regWrite32(uint64_t base, uint32_t offset, uint32_t value) override;
    void regWrite64(uint64_t base, uint32_t offset, uint64_t value) override;

    static uint64_t dmaReach()
    {
        return (xhost::gHost.xhci(0x10) & 1) ? (1ULL << 40) : (1ULL << 32);
    }  // HCCPARAMS1.AC64

    IOMapper *copyMapper() override
    {
        IOMapper *mapper = switch_usb_dma_mapper(dmaReach());
        if (mapper)
            mapper->retain();
        return mapper;
    }
};
