// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchXHCI: the Tegra X1 XUSB host controller under Apple's original AppleUSBXHCI (kits/SwitchABI).
//
// iPadOS drives USB host through IOUSBHostFamily + AppleUSBXHCI (generic xHCI: rings, slots, ports, hubs, power) with
// a thin SoC subclass (AppleNubT8011USBXHCI on iPad7,1). Once the falcon firmware runs, Tegra's host is a plain xHCI
// 1.0 (Linux drives it with the generic xhci-hcd), so SwitchXHCI is the same kind of thin subclass: XusbHost.cpp does
// the Tegra bring-up, a provider nub carries the register window and the host interrupt, and SwitchXHCI keeps what is
// Tegra's own: DMA coherency (ring buffers allocated non-cacheable, copyMapper() -> UsbDmaMapper.cpp) and the falcon
// mailbox (its own interrupt). Everything above (enumeration, IOUSBHostDevice/Interface publication, hubs, HID and
// mass storage class drivers) is Apple's unchanged code.
//
// AppleUSBXHCI::start refuses a controller with fewer than three interrupters (HCSPARAMS1.MaxIntrs). With one provider
// interrupt it runs them all on the shared line (legacy interrupters), so when the Tegra firmware reports fewer, we
// report three and keep the missing register sets in a shadow (the regRead32/regRead64/regWrite32/regWrite64
// overrides: AppleUSBXHCIInterrupter::enable reads ERSTBA and ERDP back through regRead64); which case ran is logged.
#include "SwitchXHCI.h"
#include "SwitchUSB.h"
#include "SwitchXHCIServices.h"
#include "XusbLanes.h"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSKext.h>
#include "../ArmGIC/SwitchInterrupts.h"

extern "C" {
OSReturn switch_kernel_classes(void (*initialize)());
void switchxhci_static_init(void) __asm__("__GLOBAL__sub_I_SwitchXHCI.cpp");
}

xhost::Controller xhost::gHost;

using xhost::gHost;
using xhost::kRequiredInterrupters;

OSDefineMetaClassAndStructors(SwitchXHCI, AppleUSBXHCI);
SwitchXHCI *SwitchXHCI::running;

bool SwitchXHCI::start(IOService *provider)
{
    hardwareInterrupters = (gHost.xhci(4) >> 8) & 0x7ff;
    runtimeOffset = gHost.runtimeOffset();  // read once: regRead32 also runs in the primary interrupt filter
    operationalOffset = gHost.capLength();
    // Controller structures non-cacheable: the write-combine cache mode (normal non-cacheable) in place of the one
    // AppleUSBHostController::init picked (AppleNubT8011USBXHCI clears it: its fabric is coherent; ours is not).
    setRingBufferOptions((ringBufferOptions() & ~static_cast<uint32_t>(kIOMapCacheMask)) |
                         static_cast<uint32_t>(kIOMapWriteCombineCache));
    // Without the mapper AppleUSBXHCI would run on (copyMapper NULL is no error to it) there is no cache maintenance.
    if (!switch_usb_dma_mapper(dmaReach()))
        return false;

    tegraLoop = IOWorkLoop::workLoop();
    check = tegraLoop ? IOTimerEventSource::timerEventSource(
                            this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &SwitchXHCI::checkFired))
                      : nullptr;
    if (!check || tegraLoop->addEventSource(check) != kIOReturnSuccess)
        return false;
    IOService *nub = switch_interrupt_nub(xhost::kMailboxIntid, false);
    mailbox = IOFilterInterruptEventSource::filterInterruptEventSource(this, &SwitchXHCI::mailboxAction,
                                                                       &SwitchXHCI::mailboxFilter, nub, 0);
    if (!mailbox || tegraLoop->addEventSource(mailbox) != kIOReturnSuccess) {
        panic("SwitchUSB: cannot attach the XUSB mailbox interrupt (INTID %u)", xhost::kMailboxIntid);
    }
    mailbox->enable();
    if (!AppleUSBXHCI::start(provider))
        return false;
    devices = xhost::logDeviceArrivals(this);
    // xhci-tegra.c tegra_xusb_probe_finish: firmware messages on (USBCMD.EU3S is regWrite32's).
    if (!gHost.sendMailbox(xhost::MBOX_MSG_ENABLED, 0))
        IOLog("SwitchUSB: XUSB falcon did not take MSG_ENABLED\n");
    return true;
}

void SwitchXHCI::stop(IOService *provider)
{
    if (devices)
        devices->remove();
    devices = nullptr;
    if (check)
        check->cancelTimeout();
    if (mailbox)
        mailbox->disable();
    AppleUSBXHCI::stop(provider);
}

void SwitchXHCI::free()
{
    if (tegraLoop) {
        if (check)
            tegraLoop->removeEventSource(check);
        if (mailbox)
            tegraLoop->removeEventSource(mailbox);
    }
    OSSafeReleaseNULL(check);
    OSSafeReleaseNULL(mailbox);
    OSSafeReleaseNULL(tegraLoop);
    AppleUSBXHCI::free();
}

// Interrupter register sets (runtime base + 0x20 + 32 * n) that the hardware does not have.
uint32_t *SwitchXHCI::shadowFor(uint64_t address)
{
    if (!capBase || hardwareInterrupters >= kRequiredInterrupters)
        return nullptr;
    const uint64_t first = capBase + runtimeOffset + 0x20;
    const uint64_t from = first + 32ULL * hardwareInterrupters;
    const uint64_t to = first + 32ULL * kRequiredInterrupters;
    if (address < from || address >= to)
        return nullptr;
    const uint64_t at = address - first;
    return &shadow[at / 32][(at % 32) / 4];
}

namespace {
// XusbLanes.h's adjustments to AppleUSBXHCI's register writes: U1/U2 off on the SuperSpeed ports, an interrupt
// moderation floor. AppleUSBXHCI writes from its work loop and its interrupt filter: arithmetic only.
unsigned gSsFirst, gSsCount;
bool gSsFound;

uint32_t adjustWrite(uint32_t offset, uint32_t value, uint32_t runtime, uint32_t interrupters)
{
    if (!gSsFound) {
        const xhci_port_range ss = xhci_find_ports(
            [](void *, uint32_t at) {
                return gHost.xhci(at);
            },
            nullptr, static_cast<uint32_t>(xhost::kXhciBytes), 3);
        gSsFirst = ss.first;
        gSsCount = ss.count;
        gSsFound = true;
    }
    switch (xhci_write_kind(offset, gHost.capLength(), runtime, gSsFirst, gSsCount, interrupters)) {
    case XHCI_WRITE_SS_PORTPMSC:
        return xhci_portpmsc_ss(value);
    case XHCI_WRITE_IMOD:
        return xhci_imod_floor(value);
    case XHCI_WRITE_OTHER:
        break;
    }
    return value;
}
}  // namespace

uint32_t SwitchXHCI::regRead32(uint64_t base, uint32_t offset)
{
    if (!capBase && offset == 4)
        capBase = base;  // start reads the capability registers first
    if (const uint32_t *s = shadowFor(base + offset))
        return *s;
    uint32_t value = AppleUSBXHCI::regRead32(base, offset);
    if (capBase && base + offset == capBase + 4 && hardwareInterrupters < kRequiredInterrupters) {
        value = (value & ~(0x7ffu << 8)) | kRequiredInterrupters << 8;
    }
    if (capBase && base + offset == capBase + operationalOffset + 4)
        value = xusb_hold_usbsts(&hold, value);
    return value;
}

uint64_t SwitchXHCI::regRead64(uint64_t base, uint32_t offset)
{
    if (const uint32_t *s = shadowFor(base + offset))
        return s[0] | static_cast<uint64_t>(s[1]) << 32;
    return AppleUSBXHCI::regRead64(base, offset);
}

// USBCMD and USBSTS writes through the held Save State (XusbPorts.h xusb_hold).
uint32_t SwitchXHCI::heldWrite32(uint64_t address, uint32_t value)
{
    const uint64_t usbcmd = capBase + operationalOffset;
    if (address == usbcmd + 4) {
        xusb_hold_usbsts_write(&hold, value);
        return value;
    }
    if (address != usbcmd)
        return value;
    const bool failed = hold.sre;
    // xhci-tegra.c tegra_xusb_probe_finish: USBCMD.EU3S (MFINDEX stops in U3), on every write so that a reset or a
    // controller AppleUSBXHCI reinitializes after SC7 keeps it.
    value = xusb_hold_usbcmd(&hold, value) | XHCI_USBCMD_EU3S;
    if (hold.sre && !failed)
        IOLog("SwitchUSB: XUSB host restore after SC7: the controller's state went with the falcon, USBSTS.SRE\n");
    return value;
}

// Interrupter 0's ERSTBA written back with what the halted controller still holds: not sent (XusbPorts.h xusb_hold).
bool SwitchXHCI::keepsEventRing(uint64_t address, uint64_t value, unsigned bytes)
{
    const uint32_t erstba = runtimeOffset + XHCI_ERSTBA0;
    if (address == capBase + erstba) {
        const uint64_t current =
            bytes == 8 ? AppleUSBXHCI::regRead64(capBase, erstba) : AppleUSBXHCI::regRead32(capBase, erstba);
        return xusb_hold_keeps(&hold, value, current);
    }
    if (bytes == 4 && address == capBase + erstba + 4)
        return xusb_hold_keeps(&hold, value, AppleUSBXHCI::regRead32(capBase, erstba + 4));
    return false;
}

void SwitchXHCI::regWrite32(uint64_t base, uint32_t offset, uint32_t value)
{
    const uint64_t address = base + offset;
    if (uint32_t *s = shadowFor(address)) {
        *s = value;
        return;
    }
    if (capBase && address >= capBase && address - capBase < xhost::kXhciBytes) {
        value = adjustWrite(static_cast<uint32_t>(address - capBase), value, runtimeOffset, hardwareInterrupters);
        value = heldWrite32(address, value);
        if (keepsEventRing(address, value, 4))
            return;
    }
    AppleUSBXHCI::regWrite32(base, offset, value);
}

void SwitchXHCI::regWrite64(uint64_t base, uint32_t offset, uint64_t value)
{
    if (uint32_t *s = shadowFor(base + offset)) {
        s[0] = static_cast<uint32_t>(value);
        s[1] = static_cast<uint32_t>(value >> 32);
        return;
    }
    if (capBase && keepsEventRing(base + offset, value, 8))
        return;
    AppleUSBXHCI::regWrite64(base, offset, value);
}

// SwitchXHCI's metaclass needs the loaded original AppleUSBXHCI, so its static initializer is staged
// (Drivers.cmake switch_late_object) and runs in switchxhci_start.
bool switchxhci_start(char *why, size_t whySize)
{
    if (!gHost.map()) {
        snprintf(why, whySize, "MMIO map failed");
        return false;
    }
    if (!gHost.powerOn(why, whySize))
        return false;
    if (!gHost.loadFirmware(switch_xusb_firmware, xhost::firmwareBytes(), why, whySize))
        return false;
    // The host stack's IOResources personalities, kept out of the boot-time catalogue (emit_prelink.py
    // DEFERRED_PERSONALITIES): AppleUSBHostResources (AppleUSBHostController::start waits for it) and
    // IOUSBMassStorageResource, as their kexts' Info.plist has them. iOS 27's packet capture matches IOResources too
    // (iOS 17's matches AppleUSBHostResources, so it follows that one).
    const bool resources = xhost::addResourcePersonality("AppleUSBHostResources", "AppleUSBHostResources",
                                                         "com.apple.iokit.IOUSBHostFamily") &&
                           xhost::addResourcePersonality("IOUSBMassStorageResource", "IOUSBMassStorageResource",
                                                         "com.apple.iokit.IOUSBMassStorageDriver")
#if XNU_12377_121_6
                           && xhost::addResourcePersonality("AppleUSBHostPacketFilter", "AppleUSBHostPacketFilter",
                                                            "com.apple.driver.usb.AppleUSBHostPacketFilter", "IOBSD")
#endif
        ;
    if (!resources) {
        snprintf(why, whySize, "IOCatalogue refused the USB host resource personalities");
        return false;
    }
    static const char *const kStack[] = {"com.apple.iokit.IOUSBHostFamily", "com.apple.driver.usb.AppleUSBXHCI"};
    for (const char *kext : kStack) {
        if (OSKext::loadKextWithIdentifier(kext, false) != KERN_SUCCESS) {
            snprintf(why, whySize, "%s did not load (is it prelinked?)", kext);
            return false;
        }
    }
    // Class drivers: their personalities match what AppleUSBXHCI publishes; a missing one only loses that class.
    static const char *const kClasses[] = {
        "com.apple.driver.usb.AppleUSBHub", "com.apple.driver.usb.AppleUSBHostCompositeDevice",
        "com.apple.driver.usb.IOUSBHostHIDDevice", "com.apple.iokit.IOUSBMassStorageDriver",
        "com.apple.iokit.IOSCSIBlockCommandsDevice"};
    for (const char *kext : kClasses) {
        if (OSKext::loadKextWithIdentifier(kext, false) != KERN_SUCCESS)
            IOLog("SwitchUSB: %s did not load\n", kext);
    }
    const OSMetaClass *parent = xhost::lookupClass("AppleUSBXHCI");
    if (parent != &AppleUSBXHCI::gMetaClass || parent->getClassSize() != sizeof(AppleUSBXHCI)) {
        snprintf(why, whySize, "the loaded AppleUSBXHCI is not the " SWITCH_ABI_KIND " one");
        return false;
    }
    if (switch_kernel_classes(switchxhci_static_init) != KERN_SUCCESS ||
        xhost::lookupClass("SwitchXHCI") != &SwitchXHCI::gMetaClass ||
        SwitchXHCI::gMetaClass.getSuperClass() != parent) {
        snprintf(why, whySize, "SwitchXHCI class registration failed");
        return false;
    }
    const uint32_t interrupters = (gHost.xhci(4) >> 8) & 0x7ff;
    IOService *nub = xhost::makeNub(xhost::kXhciPA, xhost::kXhciBytes, xhost::kHostIntid);
    if (nub)
        xhost::copyPortIdentity(nub, "/arm-io/usb-xhci");
    auto *controller = OSTypeAlloc(SwitchXHCI);
    if (!nub || !controller || !controller->init(nullptr) || !controller->attach(nub)) {
        OSSafeReleaseNULL(controller);
        snprintf(why, whySize, "service setup failed");
        return false;
    }
    if (!controller->start(nub)) {  // the original start may have taken references: the controller is kept
        snprintf(why, whySize, "AppleUSBXHCI::start failed (USBSTS %#x)", gHost.xhci(gHost.capLength() + 4));
        return false;
    }
    __atomic_store_n(&SwitchXHCI::running, controller, __ATOMIC_RELEASE);
    IOLog("SwitchUSB: host controller up (AppleUSBXHCI, %u interrupter%s%s)\n", interrupters,
          interrupters == 1 ? "" : "s", interrupters < kRequiredInterrupters ? ", 3 shown to AppleUSBXHCI" : "");
    return true;
}
