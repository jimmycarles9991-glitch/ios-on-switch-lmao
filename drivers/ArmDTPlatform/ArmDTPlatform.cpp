// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Generic AArch64 Darwin platform expert for a supplied Apple-format device
// tree. It derives from XNU's real IODTPlatformExpert and relies on the
// platform's own DT root name ("device-tree") for matching. No board name,
// SoC address or vendor identity is compiled into this source.
//
// The DT must contain the nodes XNU's IODT platform path consumes:
//   /chosen with a CHRP NVRAM proxy (nvram-bank-size, nvram-proxy-data)
//   /options, /cpus, /arm-io and /defaults
// The NVRAM proxy is required so the real IODTNVRAM is published before any
// security initialization. This driver does not fabricate NVRAM or suppress
// a missing one; absent inputs make configure() fail.

#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/rtc/IORTCController.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSIterator.h>
#include <pexpert/pexpert.h>
#include "../../kits/SwitchDT/SwitchDTProperty.h"

// Observation counters describe actual published nubs, not started drivers.
extern "C" {
volatile uint32_t armdt_platform_configured = 0;
volatile uint32_t armdt_platform_child_count = 0;
volatile uint32_t armdt_platform_cpu_count = 0;
volatile uint32_t armdt_platform_bus_count = 0;
volatile uint32_t armdt_platform_defaults_count = 0;
volatile uint32_t armdt_platform_nvram_proxy = 0;
volatile uint32_t armdt_platform_device_count = 0;
}

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class ArmDTPlatform : public IODTPlatformExpert {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
OSDeclareDefaultStructors(ArmDTPlatform) public : bool configure(IOService *provider) override;
    const char *deleteList() override;
    const char *excludeList() override;
    void getUTCTimeOfDay(clock_sec_t *secs, clock_nsec_t *nsecs) override;
    void setUTCTimeOfDay(clock_sec_t secs, clock_nsec_t nsecs) override;

private:
    static bool hasNVRAMProxy(IOService *provider);
    bool createDeviceNubs(IOService *provider);
};

OSDefineMetaClassAndStructors(ArmDTPlatform, IODTPlatformExpert)

    // Serialized empty name lists: XNU's processTopLevel() treats a null pointer
    // as "every entry" (remove every child), not as "nothing". An empty list
    // genuinely selects no entry, so the supplied device tree is preserved.
    const char *ArmDTPlatform::deleteList()
{
    return "()";
}

// Top-level nodes that get no IOPlatformDevice nub. "options" is IODTNVRAM already (publishNVRAM ran first, and
// IODTNVRAM::init took the node's place in the device-tree plane): a nub made from it would take that place in turn,
// so IODeviceTree:/options, where userland reads and writes NVRAM, would be an empty IOPlatformDevice: nothing to
// read, and every write refused (sandbox "iokit-set-properties iokit-class:IOPlatformDevice").
const char *ArmDTPlatform::excludeList()
{
    return "('options')";
}

// The wall clock. XNU reads it in IOKitInitializeTime() -> clock_initialize_calendar() ->
// PEGetUTCTimeOfDay() and writes it on settimeofday() (PESetUTCTimeOfDay()); both land here. The
// board's RTC driver publishes itself as the "IORTC" resource (an IORTC, the class XNU defines for
// this), as IOKitInitializeTime() expects; without one the inherited stubs answer 0, the 1970 epoch.
static IORTC *publishedRTC()
{
    const IOService *resources = IOService::getResourceService();
    return resources ? OSDynamicCast(IORTC, resources->getProperty("IORTC")) : nullptr;
}

void ArmDTPlatform::getUTCTimeOfDay(clock_sec_t *secs, clock_nsec_t *nsecs)
{
    IORTC *rtc = publishedRTC();
    if (rtc != nullptr) {
        rtc->getUTCTimeOfDay(secs, nsecs);
    } else {
        IODTPlatformExpert::getUTCTimeOfDay(secs, nsecs);
    }
}

void ArmDTPlatform::setUTCTimeOfDay(clock_sec_t secs, clock_nsec_t nsecs)
{
    IORTC *rtc = publishedRTC();
    if (rtc != nullptr) {
        rtc->setUTCTimeOfDay(secs, nsecs);
    } else {
        IODTPlatformExpert::setUTCTimeOfDay(secs, nsecs);
    }
}

bool ArmDTPlatform::hasNVRAMProxy(IOService *provider)
{
    const IORegistryEntry *chosen = provider->childFromPath("chosen", gIODTPlane);
    if (chosen == nullptr) {
        return false;
    }
    const bool valid = switch_dt_length(chosen, "nvram-bank-size") == sizeof(uint32_t) &&
                       switch_dt_length(chosen, "nvram-proxy-data") != 0;
    chosen->release();
    return valid;
}

bool ArmDTPlatform::createDeviceNubs(IOService *provider)
{
    const IORegistryEntry *bus = provider->childFromPath("arm-io", gIODTPlane);
    if (bus == nullptr) {
        return false;
    }
    // IODTPlatformExpert's IOPlatformDevice nubs resolve resources through
    // their provider's IOPlatformExpert::getNubResources. They must therefore
    // be attached to this platform expert, not to the arm-io IOPlatformDevice
    // (ArmVirtPlatform uses the same parent relationship). The device-tree
    // parent remains arm-io, so reg/ranges resolution still follows the bus.
    // Publish every real arm-io child, not only the first pattern match. The
    // GIC lives here, and the UFS host/PHY nodes must be visible to their native
    // drivers too. processTopLevel() still uses deleteList()/excludeList().
    OSIterator *iterator = bus->getChildIterator(gIODTPlane);
    bus->release();
    if (iterator == nullptr) {
        return false;
    }
    uint32_t count = 0;
    IORegistryEntry *entry = nullptr;
    while ((entry = OSDynamicCast(IORegistryEntry, iterator->getNextObject())) != nullptr) {
        IOService *nub = createNub(entry);
        if (nub == nullptr) {
            continue;
        }
        if (!nub->attach(this)) {
            nub->release();
            continue;
        }
        nub->registerService();
        nub->release();
        count++;
    }
    iterator->release();
    armdt_platform_device_count = count;
    return true;
}

bool ArmDTPlatform::configure(IOService *provider)
{
    const IORegistryEntry *cpus = provider->childFromPath("cpus", gIODTPlane);
    const IORegistryEntry *bus = provider->childFromPath("arm-io", gIODTPlane);
    const IORegistryEntry *defaults = provider->childFromPath("defaults", gIODTPlane);
    const IORegistryEntry *options = provider->childFromPath("options", gIODTPlane);
    const bool structure = (cpus != nullptr) && (bus != nullptr) && (defaults != nullptr) && (options != nullptr);
    if (cpus != nullptr) {
        cpus->release();
    }
    if (bus != nullptr) {
        bus->release();
    }
    if (defaults != nullptr) {
        defaults->release();
    }
    if (options != nullptr) {
        options->release();
    }
    if (!structure) {
        IOLog("ArmDTPlatform: required /cpus, /arm-io, /defaults or /options node is absent\n");
        return false;
    }
    if (!hasNVRAMProxy(provider)) {
        IOLog("ArmDTPlatform: /chosen does not supply a usable CHRP NVRAM proxy\n");
        return false;
    }

    // The inherited implementation publishes real IODTNVRAM from /chosen,
    // then attaches the CPU and top-level nubs. No identity or resource is
    // invented here; failure is returned instead of replaced with a stub.
    if (!IODTPlatformExpert::configure(provider)) {
        return false;
    }

    OSIterator *iterator = getChildIterator(gIOServicePlane);
    if (iterator == nullptr) {
        return false;
    }
    uint32_t count = 0;
    uint32_t cpuCount = 0;
    uint32_t busCount = 0;
    uint32_t defaultsCount = 0;
    const IORegistryEntry *child = nullptr;
    while ((child = OSDynamicCast(IORegistryEntry, iterator->getNextObject())) != nullptr) {
        count++;
        if (switch_dt_string_is(child, "device_type", "cpu")) {
            cpuCount++;
        }
        if (strcmp(child->getName(), "arm-io") == 0) {
            busCount++;
        }
        if (strcmp(child->getName(), "defaults") == 0) {
            defaultsCount++;
        }
    }
    iterator->release();
    armdt_platform_child_count = count;
    armdt_platform_cpu_count = cpuCount;
    armdt_platform_bus_count = busCount;
    armdt_platform_defaults_count = defaultsCount;
    armdt_platform_nvram_proxy = 1;
    armdt_platform_configured = 1;
    // Publish the bus child devices (interrupt controller, VirtIO MMIO, ...)
    // as real IOPlatformDevice nubs. No driver is started here.
    if (!createDeviceNubs(provider)) {
        return false;
    }
    registerService();
    return true;
}
