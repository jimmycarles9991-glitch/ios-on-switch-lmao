// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchBatteryStore.h's NVRAM: IODTNVRAM's variables, committed with sync(), which SwitchNVRAM writes to the Data
// volume a moment later. 24B119 only.
#include <IOKit/IOLib.h>
#include <IOKit/IONVRAM.h>
#include <IOKit/IORegistryEntry.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include "SwitchBatteryStore.h"

#if SWITCH_IPSW_KIND_24B119
namespace {

IODTNVRAM *nvram()
{
    IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
    IODTNVRAM *nvram = OSDynamicCast(IODTNVRAM, options);
    if (!nvram)
        OSSafeReleaseNULL(options);
    return nvram;
}

}  // namespace

bool switch_battery_nvram_read(const char *key, uint8_t *bytes, size_t *length)
{
    IODTNVRAM *store = nvram();
    if (!store)
        return false;
    OSObject *value = store->copyProperty(key);
    const OSData *data = OSDynamicCast(OSData, value);
    const bool ok = data && data->getLength() <= *length;
    if (ok) {
        *length = data->getLength();
        __builtin_memcpy(bytes, data->getBytesNoCopy(), *length);
    }
    OSSafeReleaseNULL(value);
    store->release();
    return ok;
}

bool switch_battery_nvram_write(const char *key, const uint8_t *bytes, size_t length)
{
    IODTNVRAM *store = nvram();
    if (!store)
        return false;
    OSData *data = OSData::withBytes(bytes, static_cast<unsigned int>(length));
    const OSSymbol *name = OSSymbol::withCString(key);
    const bool ok = data && name && store->setProperty(name, data);
    static bool logged;
    if (ok) {
        store->sync();
    } else if (!logged) {
        logged = true;
        IOLog("SwitchBattery: NVRAM %s not written (%zu bytes); later failures are not logged\n", key, length);
    }
    OSSafeReleaseNULL(name);
    OSSafeReleaseNULL(data);
    store->release();
    return ok;
}
#endif
