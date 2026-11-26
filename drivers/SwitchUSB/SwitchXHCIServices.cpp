// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchXHCI's IOKit plumbing; see SwitchXHCIServices.h.
#include "SwitchXHCIServices.h"
#include <IOKit/IOCatalogue.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSSymbol.h>
#include "../ArmGIC/SwitchInterrupts.h"

namespace xhost {

const OSMetaClass *lookupClass(const char *name)
{
    const OSSymbol *symbol = OSSymbol::withCString(name);
    const OSMetaClass *meta = symbol ? OSMetaClass::getMetaClassWithName(symbol) : nullptr;
    OSSafeReleaseNULL(symbol);
    return meta;
}

bool addResourcePersonality(const char *ioClass, const char *category, const char *bundle, const char *resource)
{
    OSDictionary *personality = OSDictionary::withCapacity(5);
    bool ok = personality != nullptr;
    const char *const pairs[][2] = {{"IOClass", ioClass},
                                    {"IOMatchCategory", category},
                                    {"IOProviderClass", "IOResources"},
                                    {"IOResourceMatch", resource},
                                    {"CFBundleIdentifier", bundle}};
    for (const auto &pair : pairs) {
        const OSSymbol *value = ok ? OSSymbol::withCString(pair[1]) : nullptr;
        ok = (value != nullptr) && personality->setObject(pair[0], value);
        OSSafeReleaseNULL(value);
    }
    const OSObject *entries[] = {personality};
    OSArray *array = ok ? OSArray::withObjects(entries, 1) : nullptr;
    ok = (array != nullptr) && gIOCatalogue->addDrivers(array, true);
    OSSafeReleaseNULL(array);
    OSSafeReleaseNULL(personality);
    return ok;
}

IOService *makeNub(uint64_t registers, uint64_t bytes, uint32_t intid)
{
    // NOLINTBEGIN(clang-analyzer-osx.cocoa.RetainCount): switch_interrupt_nub lends the registry's nub
    const IOService *gic = switch_interrupt_nub(intid, false);
    const OSObject *controllers = gic ? gic->copyProperty(gIOInterruptControllersKey) : nullptr;
    const OSObject *specifiers = gic ? gic->copyProperty(gIOInterruptSpecifiersKey) : nullptr;
    // NOLINTEND(clang-analyzer-osx.cocoa.RetainCount)
    OSDictionary *dict = OSDictionary::withCapacity(4);
    const IODeviceMemory *window = IODeviceMemory::withRange(registers, bytes);
    const OSObject *ranges[] = {window};
    OSArray *memory = window ? OSArray::withObjects(ranges, 1) : nullptr;
    IOService *nub = OSTypeAlloc(IOService);
    bool ok = (controllers != nullptr) && (specifiers != nullptr) && (dict != nullptr) && (memory != nullptr) &&
              (nub != nullptr);
    if (ok) {
        dict->setObject(gIOInterruptControllersKey, controllers);
        dict->setObject(gIOInterruptSpecifiersKey, specifiers);
        ok = nub->init(dict) && nub->attach(IOService::getResourceService());
    }
    if (ok) {
        nub->setDeviceMemory(memory);
        nub->setName("usb-xhci");
    }
    OSSafeReleaseNULL(controllers);
    OSSafeReleaseNULL(specifiers);
    OSSafeReleaseNULL(dict);
    OSSafeReleaseNULL(window);
    OSSafeReleaseNULL(memory);
    if (!ok)
        OSSafeReleaseNULL(nub);
    return nub;
}

}  // namespace xhost
