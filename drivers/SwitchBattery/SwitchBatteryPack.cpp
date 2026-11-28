// SPDX-License-Identifier: BSD-2-Clause-Patent
// The AppleSmartBattery service tree iPadOS 27's dasd, powerd, batteryintelligenced and thermalmonitord read (SwitchBatteryPack.h).
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include "SwitchBatteryPack.h"

#if SWITCH_IPSW_KIND_24B119
OSDefineMetaClassAndStructors(AppleSmartBattery, IOPMPowerSource);
OSDefineMetaClassAndStructors(AppleSmartBatteryBank, IOService);
OSDefineMetaClassAndStructors(AppleSmartBatteryPack, IOService);

namespace {

void setNumber(OSDictionary *dict, const char *key, int64_t value)
{
    if (OSNumber *n = OSNumber::withNumber(static_cast<unsigned long long>(value), 32)) {
        dict->setObject(key, n);
        n->release();
    }
}

void setNumber(IOService *service, const char *key, int64_t value)
{
    if (OSNumber *n = OSNumber::withNumber(static_cast<unsigned long long>(value), 32)) {
        service->setProperty(key, n);
        n->release();
    }
}

}  // namespace

OSDictionary *switch_battery_pack_data(const SwitchPackReading &r, const OSString *serial)
{
    OSDictionary *data = OSDictionary::withCapacity(13);
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
    setNumber(data, "InstantAmperage", r.instantMilliamps);
    setNumber(data, "ChargeAccum", r.chargeAccumMah);
    setNumber(data, "CycleCount", r.cycles);
    setNumber(data, "StateOfCharge", r.percent);
    setNumber(data, "FullyCharged", r.full ? 1 : 0);
    setNumber(data, "VirtualTemperature", r.centiCelsius);
    if (r.dateOfFirstUse) {
        if (OSNumber *n = OSNumber::withNumber(r.dateOfFirstUse, 64)) {
            data->setObject("DateOfFirstUse", n);
            n->release();
        }
    }
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
    auto *bank = OSTypeAlloc(AppleSmartBatteryBank);
    if (bank && bank->init() && bank->attach(pack)) {
        pack->bank = bank;
        setNumber(bank, "PackID", 0);
    } else {
        IOLog("SwitchBattery: AppleSmartBatteryBank not published\n");
        OSSafeReleaseNULL(bank);
    }
    setNumber(pack, "ID", 0);
    setNumber(pack, "BankCount", pack->bank ? 1 : 0);
    pack->update(r, serial);
    if (pack->bank)
        pack->bank->registerService();
    pack->registerService();
    return pack;
}

void AppleSmartBatteryPack::update(const SwitchPackReading &r, const OSString *serial)
{
    if (OSDictionary *data = switch_battery_pack_data(r, serial)) {
        setProperty("BatteryData", data);
        if (bank)
            bank->setProperty("BatteryData", data);
        data->release();
    }
    if (bank) {
        setNumber(bank, "Voltage", r.millivolts);
        setNumber(bank, "InstantAmperage", r.instantMilliamps);
        setNumber(bank, "Temperature", r.centiCelsius);
        setNumber(bank, "AppleRawCurrentCapacity", r.remainingMah);
        setNumber(bank, "AppleRawMaxCapacity", r.fullMah);
    }
}

void AppleSmartBatteryPack::unpublish(IOService *battery)
{
    if (bank) {
        bank->detach(this);
        OSSafeReleaseNULL(bank);
    }
    detach(battery);
}
#endif
