// SPDX-License-Identifier: BSD-2-Clause-Patent
// iPadOS 27's battery service tree. On J181, AppleSmartBatteryManagerEmbedded publishes the battery as an
// AppleSmartBattery (the IOPMPowerSource), an AppleSmartBatteryPack under it and an AppleSmartBatteryBank under the
// pack, and 24B119's readers find each by class name:
//   dasd   +[_DASBatteryTemperatureReader maxBatteryTemperatureAcrossPacks], once for every activity it scores: each
//          pack's BatteryData.VirtualTemperature. With no pack it logs "No match for AppleSmartBatteryPack IOService".
//   powerd the pack whose "ID" is 0, then its "BankCount" banks: the pack's AppleSmartBatteryBank children in order,
//          each one's BatteryData (Serial, capacities, CycleCount and the rest) for battery health. Every number is
//          read as SInt32 (FullyCharged too, so it is 0 or 1, not a boolean). With no bank it logs
//          "_findBankEntryByIndex: AppleSmartBatteryBank[0] not found" at every battery update. The battery's
//          BatteryPackCount sizes its trusted battery data.
//   batteryintelligenced  the battery's BatteryPackCount, every pack's BatteryData (VirtualTemperature,
//          NominalChargeCapacity, DesignCapacity, InstantAmperage, ChargeAccum and Voltage, a fault line for each one
//          missing) and the banks whose PackID is the pack's ID. The battery's UpdateTime is read before and after and
//          the whole read retried while it differs or is missing. With no AppleSmartBattery it logs "No match for
//          AppleSmartBattery IOService".
//   thermalmonitord  matches AppleSmartBatteryBank (initPowerSource) and reads top-level Voltage, InstantAmperage,
//          Temperature, AppleRawCurrentCapacity, AppleRawMaxCapacity and BatteryData (getGasGaugeAveragePower logs
//          "<Error> could not find Voltage property" without top-level Voltage).
// 21H461's daemons read none of this, so only a 24B119 kernel publishes it. The classes keep Apple's names because
// IOServiceMatching and IOObjectConformsTo match on them, as AppleKeyStore does in NativeKeyStoreEvents.
#pragma once
// The pack is chosen on SWITCH_IPSW_KIND_24B119, which an undefined macro silently turns off: 24B119's kernel once
// shipped without it because the driver's BUILD left the kind header out.
#ifndef SWITCH_IPSW_BUILD
#error "SwitchBattery compiles with the IPSW kind's header (IPSW_KIND_FLAGS)"
#endif
#include <IOKit/IOService.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>

// Capacities and charge in mAh, voltage in mV, currents in mA (positive = charging), temperature in centi-C.
// chargeAccum: the gauge's coulomb counter (MAX17050 QH, 0.5 mAh a count), wrapping at 16 bits.
// dateOfFirstUse: powerd's, as SwitchBatteryStore.h keeps it, 0 until it has written one.
struct SwitchPackReading {
    int remainingMah, fullMah, designMah, millivolts, avgMilliamps, instantMilliamps, chargeAccumMah, centiCelsius;
    unsigned cycles, percent;
    bool full;
    uint64_t dateOfFirstUse;
};

OSDictionary *switch_battery_pack_data(const SwitchPackReading &r, const OSString *serial);

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class AppleSmartBattery : public IOPMPowerSource {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(AppleSmartBattery);
};

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class AppleSmartBatteryBank : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(AppleSmartBatteryBank);
};

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class AppleSmartBatteryPack : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(AppleSmartBatteryPack);
    AppleSmartBatteryBank *bank = nullptr;  // the Switch's pack is one bank of parallel cells

public:
    // The bank is attached and registered before the pack, so a reader that finds the pack finds its bank.
    static AppleSmartBatteryPack *publish(IOService *battery, const SwitchPackReading &r, const OSString *serial);
    void update(const SwitchPackReading &r, const OSString *serial);
    void unpublish(IOService *battery);
};
