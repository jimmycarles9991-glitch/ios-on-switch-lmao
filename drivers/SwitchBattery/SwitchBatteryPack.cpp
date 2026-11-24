// SPDX-License-Identifier: BSD-2-Clause-Patent
// The AppleSmartBatteryPack iPadOS 27's dasd and powerd read (SwitchBatteryPack.h).
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include "SwitchBatteryPack.h"

#if SWITCH_IPSW_KIND_24B119
OSDefineMetaClassAndStructors(AppleSmartBatteryPack, IOService);

namespace {

void setNumber(OSDictionary *dict, const char *key, int64_t value)
{
    if (OSNumber *n = OSNumber::withNumber(static_cast<unsigned long long>(value), 32)) {
        dict->setObject(key, n);
        n->release();
    }
}

}  // namespace

OSDictionary *switch_battery_pack_data(const SwitchPackReading &r, const OSString *serial)
{
    OSDictionary *data = OSDictionary::withCapacity(11);
    if (!data)
        return nullptr;
    if (serial)
        data->setObject("Serial", serial);
    setNumber(data, "RemainingCapacity", r.remainingMah);
    setNumber(data, "MaxCapacity", r.fullMah);
    setNumber(data, "NominalChargeCapacity", r.fullMah);
    setNumber(data, "DesignCapacity", r.designMah);
    setNumber(data, "Voltage", r.millivolts);
    setNumber(data, "AverageCurrent", r.avgMilliamps);
    setNumber(data, "CycleCount", r.cycles);
    setNumber(data, "StateOfCharge", r.percent);
    setNumber(data, "FullyCharged", r.full ? 1 : 0);
    setNumber(data, "VirtualTemperature", r.centiCelsius);
    return data;
}

AppleSmartBatteryPack *AppleSmartBatteryPack::publish(IOService *battery, const SwitchPackReading &r,
                                                      const OSString *serial)
{
    auto *pack = OSTypeAlloc(AppleSmartBatteryPack);
    if (!pack || !pack->init() || !pack->attach(battery)) {
        IOLog("SwitchBattery: AppleSmartBatteryPack not published\n");
        OSSafeReleaseNULL(pack);
        return nullptr;
    }
    if (OSNumber *id = OSNumber::withNumber(0ULL, 32)) {
        pack->setProperty("ID", id);
        id->release();
    }
    pack->update(r, serial);
    pack->registerService();
    return pack;
}

void AppleSmartBatteryPack::update(const SwitchPackReading &r, const OSString *serial)
{
    if (OSDictionary *data = switch_battery_pack_data(r, serial)) {
        setProperty("BatteryData", data);
        data->release();
    }
}
#endif
