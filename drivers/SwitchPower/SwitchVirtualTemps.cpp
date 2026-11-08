// SPDX-License-Identifier: BSD-2-Clause-Patent
// AppleVTempDispatcher, the service thermalmonitord hands its virtual temperatures to (J120: AppleEmbeddedTempSensor's,
// on /arm-io/vtempctl0). Every 5 s CLTM computes SoC skin, ambient and centre skin (TSBH, TS0A, TSFD: CONTRACT.md) and
// sets each on this service with IORegistryEntrySetCFProperty, named by its sensor, valued in 16.16 fixed-point degrees
// C (thermalmonitord 0x10000f688). The service keeps the last value of each in "VirtualTemperatures" for the board's
// own thermal decisions; a value of another kind is refused with one line per process.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include "../../kits/SwitchRefusal/SwitchRefusal.h"
#include "SwitchPower.h"

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class AppleVTempDispatcher : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(AppleVTempDispatcher);
    IOLock *lock = nullptr;
    OSDictionary *temperatures = nullptr;

public:
    bool start(IOService *provider) override;
    void free() override;
    IOReturn setProperties(OSObject *properties) override;
};

OSDefineMetaClassAndStructors(AppleVTempDispatcher, IOService);

bool AppleVTempDispatcher::start(IOService *provider)
{
    lock = IOLockAlloc();
    temperatures = OSDictionary::withCapacity(4);
    if (!lock || !temperatures || !IOService::start(provider))
        return false;
    setProperty("VirtualTemperatures", temperatures);
    registerService();
    return true;
}

void AppleVTempDispatcher::free()
{
    if (lock)
        IOLockFree(lock);
    OSSafeReleaseNULL(temperatures);
    IOService::free();
}

IOReturn AppleVTempDispatcher::setProperties(OSObject *properties)
{
    const OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
    if (!dict || !dict->getCount()) {
        switch_refusal("AppleVTempDispatcher", "not a dictionary", "setProperties without sensor values");
        return kIOReturnBadArgument;
    }
    OSCollectionIterator *keys = OSCollectionIterator::withCollection(dict);
    if (!keys)
        return kIOReturnNoMemory;
    IOReturn result = kIOReturnSuccess;
    IOLockLock(lock);
    OSDictionary *next = OSDictionary::withDictionary(temperatures, temperatures->getCount() + dict->getCount());
    while (const OSSymbol *key = OSDynamicCast(OSSymbol, keys->getNextObject())) {
        const OSNumber *value = OSDynamicCast(OSNumber, dict->getObject(key));
        if (!value) {
            switch_refusal("AppleVTempDispatcher", "not a number", "%s is not a fixed-point temperature",
                           key->getCStringNoCopy());
            result = kIOReturnBadArgument;
            continue;
        }
        if (next)
            next->setObject(key, value);
    }
    if (next) {
        OSSafeReleaseNULL(temperatures);
        temperatures = next;
        setProperty("VirtualTemperatures", temperatures);
    }
    IOLockUnlock(lock);
    keys->release();
    return next ? result : kIOReturnNoMemory;
}

extern "C" int switch_virtual_temps_start(void)
{
    static bool started;
    if (started)
        return 0;
    IOService *resources = IOService::getResourceService();
    AppleVTempDispatcher *dispatcher = OSTypeAlloc(AppleVTempDispatcher);
    if (!resources || !dispatcher || !dispatcher->init() || !dispatcher->attach(resources)) {
        OSSafeReleaseNULL(dispatcher);
        IOLog("SwitchVirtualTemps: AppleVTempDispatcher not published\n");
        return -1;
    }
    if (!dispatcher->start(resources)) {
        dispatcher->detach(resources);
        dispatcher->release();
        IOLog("SwitchVirtualTemps: AppleVTempDispatcher did not start\n");
        return -1;
    }
    dispatcher->release();
    started = true;
    return 0;
}
