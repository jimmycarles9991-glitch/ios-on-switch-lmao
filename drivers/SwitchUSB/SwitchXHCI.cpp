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
// The falcon has one interrupter (HCSPARAMS1.MaxIntrs 1). AppleUSBXHCI::start's demand for three and every TRB's
// Interrupter Target of 1 or 2 are patched to one and 0 at the prelink (kits/SwitchABI/kinds/<kind>/sites). iOS 17's
// controller still builds three interrupters; their missing register sets are a shadow here (regRead32/regRead64/
// regWrite32/regWrite64: AppleUSBXHCIInterrupter::enable reads ERSTBA and ERDP back through regRead64).
#include "SwitchXHCI.h"
#include "SwitchUSB.h"
#include "SwitchUSBPort.h"
#include "SwitchXHCIServices.h"
#include "XusbLanes.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <libkern/c++/OSKext.h>
#if DEVELOPMENT || DEBUG
#include <os/log.h>
#endif
#include "../ArmGIC/SwitchInterrupts.h"

extern "C" {
OSReturn switch_kernel_classes(void (*initialize)());
void switchxhci_static_init(void) __asm__("__GLOBAL__sub_I_SwitchXHCI.cpp");
}

xhost::Controller xhost::gHost;

using xhost::gHost;

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
#if !XNU_12377_121_6
    // 21H461's internal bounce commands stay prepared; plain client buffers override their cache mode at allocation.
    setClientBufferOptions((clientBufferOptions() & ~static_cast<uint32_t>(kIOMapCacheMask)) |
                           static_cast<uint32_t>(kIOMapWriteCombineCache));
#endif
    // Without the mapper AppleUSBXHCI would run on (copyMapper NULL is no error to it) there is no cache maintenance.
    if (!switch_usb_dma_mapper(dmaReach()))
        return false;

    tegraLoop = IOWorkLoop::workLoop();
    if (!tegraLoop)
        return false;
    IOService *nub = switch_interrupt_nub(xhost::kMailboxIntid, false);
    mailbox = IOFilterInterruptEventSource::filterInterruptEventSource(this, &SwitchXHCI::mailboxAction,
                                                                       &SwitchXHCI::mailboxFilter, nub, 0);
    if (!mailbox || tegraLoop->addEventSource(mailbox) != kIOReturnSuccess) {
        panic("SwitchUSB: cannot attach the XUSB mailbox interrupt (INTID %u)", xhost::kMailboxIntid);
    }
    commandGate = IOCommandGate::commandGate(this);
    if (!commandGate || tegraLoop->addEventSource(commandGate) != kIOReturnSuccess)
        return false;
    mboxReplyTimer = IOTimerEventSource::timerEventSource(this, &SwitchXHCI::mboxReplyFired);
    if (!mboxReplyTimer || tegraLoop->addEventSource(mboxReplyTimer) != kIOReturnSuccess)
        return false;
    mailbox->enable();
#if !XNU_12377_121_6
    nameUSBCPort(this);
#endif
    if (!AppleUSBXHCI::start(provider))
        return false;
    // For powerStateWillChangeTo's system wake. 24B119's AppleUSBHostController registers too (it reads the root
    // domain's changes in its own powerStateWillChangeTo); IOServicePM keeps one entry per driver.
    getPMRootDomain()->registerInterestedDriver(this);
    // xhci-tegra.c tegra_xusb_probe_finish: firmware messages on (USBCMD.EU3S is regWrite32's).
    if (!enableMessages())
        IOLog("SwitchUSB: XUSB falcon did not take MSG_ENABLED\n");
    return true;
}

#if !XNU_12377_121_6
// 21H461's plain client descriptors map per transfer; its persistent DMA buffers still use the controller's WC mode.
IOBufferMemoryDescriptor *SwitchXHCI::createIOBuffer(IOOptionBits options, uint64_t capacity)
{
    if ((options & kIOMapCacheMask) == 0u)
        options |= kIOMapCopybackCache;
    return AppleUSBXHCI::createIOBuffer(options, capacity);
}
#endif

void SwitchXHCI::stop(IOService *provider)
{
    if (mailbox)
        mailbox->disable();
    if (tegraLoop) {
        tegraLoop->runAction(
            [](OSObject *owner, void *, void *, void *, void *) {
                auto *self = static_cast<SwitchXHCI *>(owner);
                if (self->mboxReplyTimer)
                    self->mboxReplyTimer->cancelTimeout();
                if (xusb_mbox_deferred_cancel(&self->deferredReply) && self->commandGate)
                    self->commandGate->commandWakeup(&self->deferredReply, false);
                return kIOReturnSuccess;
            },
            this);
    }
    getPMRootDomain()->deRegisterInterestedDriver(this);
    AppleUSBXHCI::stop(provider);
}

void SwitchXHCI::free()
{
    if (tegraLoop && mailbox)
        tegraLoop->removeEventSource(mailbox);
    if (tegraLoop && mboxReplyTimer)
        tegraLoop->removeEventSource(mboxReplyTimer);
    if (tegraLoop && commandGate)
        tegraLoop->removeEventSource(commandGate);
    OSSafeReleaseNULL(mailbox);
    OSSafeReleaseNULL(mboxReplyTimer);
    OSSafeReleaseNULL(commandGate);
    OSSafeReleaseNULL(tegraLoop);
    AppleUSBXHCI::free();
}

// What IOPort::copyParentPortForService looks for on a service or its provider: the USB-C port's IOPort
// (SwitchUSBPort.h).
void SwitchXHCI::nameUSBCPort(IORegistryEntry *entry)
{
    uint32_t type = kSwitchUSBCPortType;
    uint32_t number = kSwitchUSBCPortNumber;
    entry->setProperty("port-type", &type, sizeof(type));
    entry->setProperty("port-number", &number, sizeof(number));
}

// Only the USB-C connector's root ports exist: pad 0's USB 2.0 port and SS port 0's. The other seven have no connector;
// createPorts powers them off. Each is made as base createPort makes a port, from its 16 bytes of operational
// registers and its protocol copy, but as a USB-C port. iOS 27: the plain port with the host port type USB-C in its
// property dictionary, labelled with the IOPort it follows. iOS 17, whose plain ports have no IOPort path: the Type-C
// port Apple's T8011 controller makes for a Type-C connector, which finds the IOPort through the controller (start).
IOService *SwitchXHCI::createPort(void *protocol, uint32_t port, uint32_t)
{
    const bool superSpeed = port == gHost.ssPort();
    if (!superSpeed && port != gHost.otgPort())
        return nullptr;
    IODeviceMemory *window = getProvider()->getDeviceMemoryWithIndex(0);
    IODeviceMemory *registers =
        window ? IODeviceMemory::withSubRange(window, xhci_portsc_offset(gHost.capLength(), port), 0x10) : nullptr;
    if (!registers)
        return nullptr;
#if XNU_12377_121_6
    constexpr uint32_t kHostPortTypeUSBC = 5;  // tIOUSBHostPortType; usb-port-type 4 in the dictionary
    [[maybe_unused]] constexpr const char *kGiven = "host port type 5, USB-C";
    OSDictionary *properties = switch_original_xhci_port_properties(port, kHostPortTypeUSBC, 0);
    IOService *created = nullptr;
    if (properties) {
        created = superSpeed ? switch_original_xhci_usb3_port(registers, protocol, properties)
                             : switch_original_xhci_usb2_port(registers, protocol, properties);
        properties->release();
    }
    if (created)
        nameUSBCPort(created);
#else
    [[maybe_unused]] constexpr const char *kGiven = "Type-C port class";
    IOService *created = superSpeed ? switch_original_xhci_typec_usb3_port(registers, protocol, port, 0)
                                    : switch_original_xhci_typec_usb2_port(registers, protocol, port, 0);
#endif
    registers->release();
    if (created && created->attach(this)) {
        if (created->start(this)) {
#if DEVELOPMENT || DEBUG
            os_log(OS_LOG_DEFAULT, "SwitchUSB: root port %u created as %s (%s, %s)", port,
                   created->getMetaClass()->getClassName(), superSpeed ? "SuperSpeed" : "USB 2.0", kGiven);
#endif
            return created;
        }
        created->detach(this);
    }
    OSSafeReleaseNULL(created);
    IOLog("SwitchUSB: USB-C root port %u did not start\n", port);
    return nullptr;
}

// Interrupter register sets (runtime base + 0x20 + 32 * n) that the hardware does not have.
uint32_t *SwitchXHCI::shadowFor(uint64_t address)
{
#if XNU_12377_121_6
    (void)address;
    return nullptr;
#else
    if (!capBase || hardwareInterrupters >= xhost::kAppleInterrupters)
        return nullptr;
    const uint64_t first = capBase + runtimeOffset + 0x20;
    const uint64_t from = first + 32ULL * hardwareInterrupters;
    const uint64_t to = first + 32ULL * xhost::kAppleInterrupters;
    if (address < from || address >= to)
        return nullptr;
    const uint64_t at = address - first;
    return &shadow[at / 32][(at % 32) / 4];
#endif
}

namespace {
// XusbLanes.h's interrupt moderation floor on AppleUSBXHCI's IMOD writes. AppleUSBXHCI writes from its work loop and
// its interrupt filter: arithmetic only.
uint32_t adjustWrite(uint32_t offset, uint32_t value, uint32_t runtime, uint32_t interrupters)
{
    return xhci_is_imod(offset, runtime, interrupters) ? xhci_imod_floor(value) : value;
}
}  // namespace

uint32_t SwitchXHCI::regRead32(uint64_t base, uint32_t offset)
{
    if (!capBase && offset == 4)
        capBase = base;  // start reads the capability registers first
    if (const uint32_t *s = shadowFor(base + offset))
        return *s;
    uint32_t value = AppleUSBXHCI::regRead32(base, offset);
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
    // The controller's reset leaves the SuperSpeed port powered, so the falcon's SuperSpeed interface reset is owed
    // again (SwitchXHCIEvents.cpp); the role thread does it once the controller runs.
    if (value & XHCI_USBCMD_HCRST) {
        __atomic_store_n(&sspiDue, true, __ATOMIC_RELEASE);
        __atomic_store_n(&stateLost, false, __ATOMIC_RELEASE);
    }
    if ((value & XHCI_USBCMD_RS) && __atomic_load_n(&sspiDue, __ATOMIC_ACQUIRE))
        switch_usb_reconsider();
    if (hold.sre && !failed)
        IOLog("SwitchUSB: XUSB host restore after SC7: the controller's state went with the falcon, USBSTS.SRE\n");
    return value;
}

// Interrupter 0's ERSTBA written back with what the halted controller still holds: not sent (XusbPorts.h xusb_hold).
bool SwitchXHCI::keepsEventRing(uint64_t address, uint64_t value, unsigned bytes)
{
    const uint32_t erstba = runtimeOffset + XHCI_ERSTBA0;
    const uint64_t offset = address - capBase;
    if (offset != erstba && !(bytes == 4 && offset == erstba + 4))
        return false;
    if (!xusb_hold_retained(&hold))
        return false;
    return value == (bytes == 8 ? AppleUSBXHCI::regRead64(capBase, erstba)
                                : AppleUSBXHCI::regRead32(capBase, static_cast<uint32_t>(offset)));
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
    // The host stack's IOResources personalities, kept out of the boot-time catalogue (BUILD.bazel
    // prelink_deferred): AppleUSBHostResources (AppleUSBHostController::start waits for it) and
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
    IOLog("SwitchUSB: host controller up (AppleUSBXHCI, %u interrupter%s)\n", interrupters,
          interrupters == 1 ? "" : "s");
    return true;
}
