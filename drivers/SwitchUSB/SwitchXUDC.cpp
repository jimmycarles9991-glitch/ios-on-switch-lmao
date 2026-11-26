// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchXUDC (SwitchXUDC.h): lifecycle, registration, the bus, port ownership and system sleep.
#include "SwitchXUDC.h"
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSData.h>
#include "../ArmGIC/SwitchInterrupts.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "SwitchUSB.h"

extern "C" {
OSReturn switch_kernel_classes(void (*initialize)());
void switchxudc_static_init(void) __asm__("__GLOBAL__sub_I_SwitchXUDC.cpp");
}

OSDefineMetaClassAndStructors(SwitchXUDC, IOUSBDeviceController) SwitchXUDC *SwitchXUDC::running;

namespace {
bool gHostVbus, gLaneReady;  // switchxudc_set_vbus
}  // namespace

// Everything that can fail without side effects on the family happens here, before start().
bool SwitchXUDC::begin(IOService *provider, char *why, size_t whySize)
{
    stage = Stage::Idle;
    speed = kSpeedNone;
    if (!hw.map()) {
        snprintf(why, whySize, "MMIO map failed");
        return false;
    }
    loop = IOWorkLoop::workLoop();
    idleHoldCall = thread_call_allocate_with_options(&SwitchXUDC::applyIdleHold, this, THREAD_CALL_PRIORITY_KERNEL,
                                                     THREAD_CALL_OPTIONS_ONCE);
    statsBegin();
    scratchRuns = IONew(Run, kMaxRuns);
    scratchPieces = IONew(Piece, kMaxPieces);
    scratchIova = IONew(Run, kMaxRuns);
    if (!loop || !scratchRuns || !scratchPieces || !scratchIova || !ep0Buffer.allocate(kEp0Bytes)) {
        snprintf(why, whySize, "out of memory");
        return false;
    }
    // The bounce pool, taken at start-up while a contiguous run below 4 GiB is easy to find (as SwitchUSB's host pool).
    // Without it every packet that cannot go direct takes the serial path.
    if (pool.allocate(kPoolBytes)) {
        poolFree = kPoolPages;
    } else {
        IOLog("SwitchUSB: no %u KiB bounce pool below 4 GiB: requests that need one go one TRB at a time\n",
              kPoolBytes / 1024);
    }
    smmuBegin();
    return hw.powerOn(why, whySize);
}

bool SwitchXUDC::start(IOService *provider)
{
    // The family builds its command gate, pool and timers on getWorkLoop().
    if (!IOUSBDeviceController::start(provider))
        return false;
    OSData *capability = OSData::withBytes(kSuperSpeedCapability, sizeof(kSuperSpeedCapability));
    const bool described = capability && deviceCapabilities() && deviceCapabilities()->setObject(capability);
    OSSafeReleaseNULL(capability);
    if (!described) {
        IOLog("SwitchUSB: no memory for the SuperSpeed device capability descriptor\n");
        return false;
    }
    attachInterrupt();  // sleeps until the GIC is up; panics if the interrupt cannot be had
    busReturn = IOTimerEventSource::timerEventSource(this, &SwitchXUDC::busReturned);
    if (!busReturn || loop->addEventSource(busReturn) != kIOReturnSuccess) {
        IOLog("SwitchUSB: no timer for re-enumeration: forceReEnumeration stays off the bus\n");
        OSSafeReleaseNULL(busReturn);
    }
    readyForHost();  // control request, USB device description defaults, registerService()
    return true;
}

void SwitchXUDC::stop(IOService *provider)
{
    if (interrupt)
        interrupt->disable();
    if (busReturn) {
        (void)endReturn();
        loop->removeEventSource(busReturn);
    }
    setBus(false);
    if (interrupt && loop)
        loop->removeEventSource(interrupt);
    IOUSBDeviceController::stop(provider);
}

void SwitchXUDC::free()
{
    OSSafeReleaseNULL(interrupt);
    OSSafeReleaseNULL(busReturn);
    for (Endpoint &ep : endpoints) {
        ep.serial.release();
        if (ep.queue)
            IODelete(ep.queue, Request, kQueueDepth);
        if (ep.tds)
            IODelete(ep.tds, Tds, 1);
        ep.queue = nullptr;
        ep.tds = nullptr;
    }
    if (scratchRuns)
        IODelete(scratchRuns, Run, kMaxRuns);
    if (scratchPieces)
        IODelete(scratchPieces, Piece, kMaxPieces);
    if (scratchIova)
        IODelete(scratchIova, Run, kMaxRuns);
    ep0Buffer.release();
    pool.release();
    OSSafeReleaseNULL(loop);
    IOUSBDeviceController::free();
}

// Interrupt (work loop context: the family's gate is held).
void SwitchXUDC::attachInterrupt()
{
    IOService *nub = switch_interrupt_nub(kXudcIntid, false);
    interrupt = IOFilterInterruptEventSource::filterInterruptEventSource(this, &SwitchXUDC::interruptOccurred,
                                                                         &SwitchXUDC::interruptFilter, nub, 0);
    if (!interrupt || loop->addEventSource(interrupt) != kIOReturnSuccess) {
        panic("SwitchUSB: cannot attach the XUDC interrupt (INTID %u)", kXudcIntid);
    }
    interrupt->enable();
}

void SwitchXUDC::goOnBus(bool on)
{
    (void)endReturn();
    wantBus = on;
    if (hostOwnsPort) {  // the host has the pad: remember the wish
        return;
    }
    setBus(on);
}

// The pad changes hands. To the host: off the bus as a cable pull would be, the family's wish kept. Back: on the bus
// again if the family wants it.
IOReturn SwitchXUDC::portOwner(bool host)
{
    if (host == hostOwnsPort)
        return kIOReturnSuccess;
    if (host) {
        wantBus = endReturn() || wantBus || onBus;
        setBus(false);
        hostOwnsPort = true;
    } else {
        hostOwnsPort = false;
        if (wantBus)
            setBus(true);
    }
    return kIOReturnSuccess;
}

// The VBUS a host gives the port (SwitchUSB.cpp from SwitchPD's attach and detach), kept for a controller that is
// not running or not on the bus yet.
extern "C" void switchxudc_set_vbus(bool present, bool laneReady)
{
    __atomic_store_n(&gHostVbus, present, __ATOMIC_RELEASE);
    __atomic_store_n(&gLaneReady, laneReady, __ATOMIC_RELEASE);
    SwitchXUDC *controller = __atomic_load_n(&SwitchXUDC::running, __ATOMIC_ACQUIRE);
    if (controller) {
        controller->getWorkLoop()->runAction(
            [](OSObject *owner, void *, void *, void *, void *) {
                static_cast<SwitchXUDC *>(owner)->vbusChanged();
                return kIOReturnSuccess;
            },
            controller);
    }
}

void SwitchXUDC::vbusChanged()
{
    if (!hostOwnsPort && !asleep && !returning)
        setBus(wantBus && __atomic_load_n(&gHostVbus, __ATOMIC_ACQUIRE) &&
               __atomic_load_n(&gLaneReady, __ATOMIC_ACQUIRE));
    if (onBus)
        hw.setVbus(__atomic_load_n(&gHostVbus, __ATOMIC_ACQUIRE));
}

extern "C" void switchxudc_set_host_owns_port(bool host)
{
    SwitchXUDC *controller = __atomic_load_n(&SwitchXUDC::running, __ATOMIC_ACQUIRE);
    if (controller) {
        controller->getWorkLoop()->runAction(
            [](OSObject *owner, void *host, void *, void *, void *) {
                return static_cast<SwitchXUDC *>(owner)->portOwner(host != nullptr);
            },
            controller, host ? controller : nullptr);
    }
}

// SC7 resets the controller, its partitions, PLLU/UTMIPLL and the pad block, and with them the event ring and every
// context: off the bus first, as a cable pull (the family gets every request back), with the interrupt off; after it
// the whole powerOn again. Its event ring starts empty, so nothing a cancel left can come
// back and every fence lifts at once.
IOReturn SwitchXUDC::systemSleep(bool entering)
{
    if (entering) {
        if (asleep)
            return kIOReturnSuccess;
        asleep = true;
        __atomic_store_n(&gLaneReady, false, __ATOMIC_RELEASE);
        const bool returned = endReturn();
        busAfterSleep = wantBus || onBus || returned;
        if (!hostOwnsPort)
            setBus(false);
        interrupt->disable();
        return kIOReturnSuccess;
    }
    if (!asleep)
        return kIOReturnSuccess;
    asleep = false;
    char why[160] = "";
    if (!hw.powerOn(why, sizeof(why))) {
        lostToSleep = true;
        IOLog("SwitchUSB: device mode off after sleep: %s\n", why);
        return kIOReturnSuccess;
    }
    for (uint32_t dci = 2; dci < kMaxDci; ++dci) {
        if (endpoints[dci].tds)
            endpoints[dci].tds->liftFence();
    }
    interrupt->enable();
    wantBus = busAfterSleep;
    if (hostOwnsPort) {
        wantBus = busAfterSleep;
    } else if (busAfterSleep) {
        setBus(true);
    }
    return kIOReturnSuccess;
}

void switchxudc_sleep(bool entering)
{
    SwitchXUDC *controller = __atomic_load_n(&SwitchXUDC::running, __ATOMIC_ACQUIRE);
    if (controller) {
        controller->getWorkLoop()->runAction(
            [](OSObject *owner, void *entering, void *, void *, void *) {
                return static_cast<SwitchXUDC *>(owner)->systemSleep(entering != nullptr);
            },
            controller, entering ? controller : nullptr);
    }
}

void SwitchXUDC::setBus(bool on)
{
    if (on == onBus)
        return;
    if (on && (lostToSleep || asleep || !__atomic_load_n(&gHostVbus, __ATOMIC_ACQUIRE) ||
               !__atomic_load_n(&gLaneReady, __ATOMIC_ACQUIRE)))
        return;
    if (on) {
        ep0Cancel(kIOReturnAborted);
        hw.initEndpoint(0, EP_CONTROL, 64, 0);
        (void)hw.reloadEndpoint(0);
        hw.connect();
        hw.setVbus(__atomic_load_n(&gHostVbus, __ATOMIC_ACQUIRE));
        onBus = true;
        if (!reportedOnBus) {
            reportedOnBus = true;
            IOLog("SwitchUSB: functions registered, pull-up on (waiting for a host)\n");
        }
        return;
    }
    onBus = false;
    hw.disconnect();
    ep0Cancel(kIOReturnAborted);
    for (uint32_t dci = 2; dci < kMaxDci; ++dci)
        cancel(dci, kIOReturnAborted);
    speed = kSpeedNone;
    suspended = false;
    setHostPresent(false);
}

// An iPad's powerd holds PreventUserIdleSystemSleep while its port's IOPortTransportStateUSB shows a device-role data
// connection (Active, DataRole 1) on AC power. Nothing here publishes that object (IOPortFamily's port controller
// makes it, and its init is not ours to call), so the attached host holds the kernel's idle-sleep assertion instead:
// the same effect on IOPMrootDomain, and a host always powers VBUS. Demand sleep is untouched.
// The assertion changes on a thread call: hostPresent changes in the event drain, on the work loop, where the root
// domain's assertion lock and PM queue are not to be waited on.
void SwitchXUDC::setHostPresent(bool present)
{
    hostPresent = present;
    if (idleWanted == present)
        return;
    idleWanted = present;
    if (idleHoldCall)
        thread_call_enter(idleHoldCall);
}

void SwitchXUDC::applyIdleHold(thread_call_param_t owner, thread_call_param_t)
{
    auto *self = static_cast<SwitchXUDC *>(owner);
    IOPMrootDomain *root = getPMRootDomain();
    const bool want = self->idleWanted;
    if (!root || want == (self->idleHold != 0))
        return;
    if (want) {
        self->idleHold = root->createPMAssertion(kIOPMDriverAssertionPreventSystemIdleSleepBit,
                                                 kIOPMDriverAssertionLevelOn, self, "SwitchUSB host attached");
    } else {
        root->releasePMAssertion(self->idleHold);
        self->idleHold = 0;
    }
}

// AppleUSBXDCI::softDisconnect clears DCTL.RUN_STOP and nothing more: the family puts us back with goOnBus(true).
void SwitchXUDC::softDisconnect(uint32_t)
{
    goOnBus(false);
}

// The family's own off-and-on (userland GoOffAndOnBus). 100 ms off so the host sees the detach, timed on the work loop
// so nothing else waits for it: on again when busReturn fires, unless the family, the host role or sleep decided
// otherwise first.
void SwitchXUDC::forceReEnumeration()
{
    if (hostOwnsPort)
        return;
    setBus(false);
    if (busReturn) {
        returning = true;
        busReturn->setTimeoutMS(100);
    }
}

// Whether forceReEnumeration's off time was running; it is not any more.
bool SwitchXUDC::endReturn()
{
    if (!returning)
        return false;
    returning = false;
    busReturn->cancelTimeout();
    return true;
}

void SwitchXUDC::busReturned(OSObject *owner, IOTimerEventSource *)
{
    auto *self = static_cast<SwitchXUDC *>(owner);
    if (self->returning)
        self->goOnBus(true);
}

// Registration: SwitchXUDC's metaclass needs the loaded original family, so its static initializer is staged
// (Drivers.cmake switch_late_object) and runs here.
namespace {
const OSMetaClass *lookupClass(const char *text)
{
    const OSSymbol *name = OSSymbol::withCString(text);
    const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    return meta;
}
}  // namespace

bool switchxudc_start(IOService *provider, char *why, size_t whySize)
{
    if (OSKext::loadKextWithIdentifier("com.apple.iokit.IOUSBDeviceFamily", false) != KERN_SUCCESS) {
        snprintf(why, whySize, "IOUSBDeviceFamily did not load (is it prelinked?)");
        return false;
    }
    if (OSKext::loadKextWithIdentifier("com.apple.driver.AppleUSBDeviceMux", false) != KERN_SUCCESS) {
        snprintf(why, whySize, "AppleUSBDeviceMux did not load (is it prelinked?)");
        return false;
    }
    const OSMetaClass *parent = lookupClass("IOUSBDeviceController");
    if (parent != &IOUSBDeviceController::gMetaClass || parent->getClassSize() != sizeof(IOUSBDeviceController)) {
        snprintf(why, whySize, "the loaded IOUSBDeviceController is not the " SWITCH_ABI_KIND " one");
        return false;
    }
    if (switch_kernel_classes(switchxudc_static_init) != KERN_SUCCESS ||
        lookupClass("SwitchXUDC") != &SwitchXUDC::gMetaClass || SwitchXUDC::gMetaClass.getSuperClass() != parent) {
        snprintf(why, whySize, "SwitchXUDC class registration failed");
        return false;
    }
    auto *controller = OSTypeAlloc(SwitchXUDC);
    if (!controller || !controller->init()) {
        OSSafeReleaseNULL(controller);
        snprintf(why, whySize, "out of memory");
        return false;
    }
    if (!controller->begin(provider, why, whySize)) {
        controller->release();
        return false;
    }
    if (!controller->attach(provider)) {
        controller->release();
        snprintf(why, whySize, "attach to %s failed", provider->getName());
        return false;
    }
    if (!controller->start(provider)) {
        controller->detach(provider);
        controller->release();
        snprintf(why, whySize, "IOUSBDeviceController::start failed");
        return false;
    }
    __atomic_store_n(&SwitchXUDC::running, controller, __ATOMIC_RELEASE);
    controller->release();  // the registry holds it
    return true;
}
