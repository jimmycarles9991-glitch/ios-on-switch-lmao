// SPDX-License-Identifier: BSD-2-Clause-Patent
// iPadOS 27's battery pack service. On J181, AppleSmartBatteryManagerEmbedded publishes an AppleSmartBatteryPack (an
// IOService) beside its IOPMPowerSource, and 24B119's readers find it by class name:
//   dasd   +[_DASBatteryTemperatureReader maxBatteryTemperatureAcrossPacks], once for every activity it scores: each
//          pack's BatteryData.VirtualTemperature. With no pack it logs "No match for AppleSmartBatteryPack IOService".
//   powerd the pack whose "ID" is 0, then BatteryData's Serial, RemainingCapacity, MaxCapacity, DesignCapacity, Voltage,
//          AverageCurrent, CycleCount, NominalChargeCapacity, FullyCharged and StateOfCharge for battery health. Each
//          key is optional and every number is read as SInt32 (FullyCharged too, so it is 0 or 1, not a boolean). With
//          no pack it logs "Failed to find battery pack with ID 0".
// 21H461's dasd and powerd read no pack, so only a 24B119 kernel publishes one. The class keeps Apple's name because
// IOServiceMatching matches on it, as AppleKeyStore does in NativeKeyStoreEvents.
#pragma once
#include <IOKit/IOService.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>

// Capacities in mAh, voltage in mV, current in mA (positive = charging), temperature in centi-C.
struct SwitchPackReading {
    int remainingMah, fullMah, designMah, millivolts, avgMilliamps, centiCelsius;
    unsigned cycles, percent;
    bool full;
};

OSDictionary *switch_battery_pack_data(const SwitchPackReading &r, const OSString *serial);

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class AppleSmartBatteryPack : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(AppleSmartBatteryPack);

public:
    static AppleSmartBatteryPack *publish(IOService *battery, const SwitchPackReading &r, const OSString *serial);
    void update(const SwitchPackReading &r, const OSString *serial);
};
