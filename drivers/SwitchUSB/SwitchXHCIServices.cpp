// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchXHCI's IOKit plumbing; see SwitchXHCIServices.h.
#include "SwitchXHCIServices.h"
#include <IOKit/IOCatalogue.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <string.h>
#include "../ArmGIC/SwitchInterrupts.h"
#include "../../kits/SwitchDT/SwitchDTProperty.h"

namespace xhost {
namespace {
constexpr const char *kForcePower = "UsbForcePower";
constexpr const char *kPortNumber = "port";
constexpr const char *kPortClass = "AppleUSBHostPort";
}  // namespace

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

IOService *waitForRootPort(IOService *controller, unsigned number, uint64_t timeoutNS)
{
    const uint32_t port = number;
    const OSSymbol *key = OSSymbol::withCString(kPortNumber);
    const OSData *value = OSData::withBytes(&port, sizeof(port));
    const OSDictionary *parent = IOService::registryEntryIDMatching(controller->getRegistryEntryID());
    OSDictionary *matching = IOService::serviceMatching(kPortClass);
    IOService *found = nullptr;
    if (key != nullptr && value != nullptr && parent != nullptr && matching != nullptr &&
        IOService::propertyMatching(key, value, matching) != nullptr &&
        matching->setObject(kIOParentMatchKey, parent)) {
        found = IOService::waitForMatchingService(matching, timeoutNS);
    }
    OSSafeReleaseNULL(matching);
    OSSafeReleaseNULL(parent);
    OSSafeReleaseNULL(value);
    OSSafeReleaseNULL(key);
    return found;
}

void describePorts(const IOService *controller, char *buffer, size_t size)
{
    size_t used = 0;
    buffer[0] = 0;
    OSIterator *children = controller->getChildIterator(gIOServicePlane);
    while (children != nullptr && used + 1 < size) {
        auto *child = OSDynamicCast(IOService, children->getNextObject());
        if (child == nullptr)
            break;
        const OSObject *value = child->copyProperty(kPortNumber);
        uint32_t number = 0;
        const bool known = switch_osdata_read_prefix(value, &number, sizeof(number));
        const unsigned power = child->getPowerState();
        const int n = known ? snprintf(buffer + used, size - used, "%s%s port %u power %u", used != 0 ? ", " : "",
                                       child->getMetaClass()->getClassName(), number, power)
                            : snprintf(buffer + used, size - used, "%s%s port ? power %u", used != 0 ? ", " : "",
                                       child->getMetaClass()->getClassName(), power);
        OSSafeReleaseNULL(value);
        if (n <= 0)
            break;
        used = used + static_cast<size_t>(n) < size ? used + static_cast<size_t>(n) : size - 1;
    }
    OSSafeReleaseNULL(children);
}

IOReturn forcePower(IOService *service, bool on)
{
    const OSNumber *value = OSNumber::withNumber(on ? 1U : 0U, 32);
    OSDictionary *request = value != nullptr ? OSDictionary::withCapacity(1) : nullptr;
    if (request == nullptr || !request->setObject(kForcePower, value))
        panic("SwitchUSB: no memory for UsbForcePower");
    const IOReturn result = service->setProperties(request);
    OSSafeReleaseNULL(request);
    OSSafeReleaseNULL(value);
    return result;
}

}  // namespace xhost
