// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchXHCI's class. SwitchXHCI.cpp is the controller (start, the connector's root ports, the register overrides, the
// bring-up), SwitchXHCIEvents.cpp its events (the falcon mailbox, the SuperSpeed interface reset, the role, SC7).
#pragma once
#include "OriginalAppleUSBXHCI.h"
#include "UsbDmaMapper.h"
#include "XusbHost.h"
#include "XusbMailbox.h"
#include "XusbPorts.h"
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>

extern "C" {
// tegra210b01_xusb_firmware (firmware.S, drivers/SwitchUSB/firmware/README.md).
extern const uint8_t switch_xusb_firmware[] __asm__("_switch_xusb_firmware");
extern const uint8_t switch_xusb_firmware_end[] __asm__("_switch_xusb_firmware_end");
}

namespace xhost {
extern Controller gHost;  // SwitchXHCI.cpp
#if !XNU_12377_121_6
// 21H461 constructs three interrupters regardless of MaxIntrs; only the missing register sets need shadowing.
constexpr uint32_t kAppleInterrupters = 3;
#endif

inline size_t firmwareBytes()
{
    return reinterpret_cast<uintptr_t>(switch_xusb_firmware_end) - reinterpret_cast<uintptr_t>(switch_xusb_firmware);
}
}  // namespace xhost

class SwitchXHCI : public AppleUSBXHCI {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchXHCI);

    IOWorkLoop *tegraLoop;  // the mailbox's; AppleUSBXHCI keeps its own loop
    volatile bool sspiDue;  // RESET_SSPI owed since the host role began or the controller's last reset
    IOFilterInterruptEventSource *mailbox;
    IOCommandGate *commandGate;  // on tegraLoop: a command's sender sleeps on it until mailboxAction has its answer
    xusb_mbox_wait commandWait;  // tegraLoop's gate
    xusb_hold hold;              // AppleUSBXHCI's work loop, and systemSleep while AppleUSBXHCI is lowered
    volatile uint32_t mailboxInterrupts;
    uint32_t unclaimed;  // primary interrupt context only
    volatile bool mailboxStorm;
#if DEVELOPMENT || DEBUG
    unsigned long powerStateFrom;  // PM's work loop: the state powerStateWillChangeTo saw, for the change's log line
#endif
    uint32_t powerState;
    volatile bool stateLost;  // SC7 took a saved state; the system's wake reports it, a controller reset forgets it
    uint32_t hardwareInterrupters, runtimeOffset, operationalOffset;
    uint64_t capBase;  // AppleUSBXHCI's mapping of the capability registers
#if !XNU_12377_121_6
    uint32_t shadow[xhost::kAppleInterrupters][8];
#endif

    static bool mailboxFilter(OSObject *owner, IOFilterInterruptEventSource *);
    static void mailboxAction(OSObject *owner, IOInterruptEventSource *, int);
    uint32_t *shadowFor(uint64_t address);
    uint32_t heldWrite32(uint64_t address, uint32_t value);
    bool keepsEventRing(uint64_t address, uint64_t value, unsigned bytes);
    static void nameUSBCPort(IORegistryEntry *entry);

public:
    static SwitchXHCI *running;

    IOWorkLoop *tegraWorkLoop() const
    {
        return tegraLoop;
    }

    bool resetSuperSpeed();
    bool sendCommand(uint32_t cmd, uint32_t data);
    bool enableMessages();

    void superSpeedResetDue()
    {
        __atomic_store_n(&sspiDue, true, __ATOMIC_RELEASE);
    }

    bool superSpeedResetOwed() const
    {
        return __atomic_load_n(&sspiDue, __ATOMIC_ACQUIRE);
    }

    bool isOff() const
    {
        return __atomic_load_n(&powerState, __ATOMIC_ACQUIRE) == 0;
    }

    static bool controllerRuns()
    {
        const uint32_t operational = xhost::gHost.capLength();
        return (xhost::gHost.xhci(operational) & XHCI_USBCMD_RS) &&
               !(xhost::gHost.xhci(operational + 4) & XHCI_USBSTS_HCH);
    }

    IOReturn systemSleep(bool entering);
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    uint32_t regRead32(uint64_t base, uint32_t offset) override;
    uint64_t regRead64(uint64_t base, uint32_t offset) override;
    void regWrite32(uint64_t base, uint32_t offset, uint32_t value) override;
    void regWrite64(uint64_t base, uint32_t offset, uint64_t value) override;
    IOService *createPort(void *protocol, uint32_t port, uint32_t protocolPort) override;
#if !XNU_12377_121_6
    IOBufferMemoryDescriptor *createIOBuffer(IOOptionBits options, uint64_t capacity) override;
#endif
    IOReturn powerStateWillChangeTo(IOPMPowerFlags capabilities, unsigned long stateNumber,
                                    IOService *whatDevice) override;
    IOReturn powerStateDidChangeTo(IOPMPowerFlags capabilities, unsigned long stateNumber,
                                   IOService *whatDevice) override;

    // HCCPARAMS1.AC64 says 64-bit, but xusb_host is an AHBSLV memory client: 32-bit addresses only (T210 TRM tables 4
    // and 5, p.30-31). What the mapper hands out with the SMMU off must stay below 4 GiB.
    static uint64_t dmaReach()
    {
        return 1ULL << 32;
    }

    IOMapper *copyMapper() override
    {
        IOMapper *mapper = switch_usb_dma_mapper(dmaReach());
        if (mapper)
            mapper->retain();
        return mapper;
    }
};
