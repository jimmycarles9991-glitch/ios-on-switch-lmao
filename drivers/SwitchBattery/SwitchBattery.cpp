// SPDX-License-Identifier: BSD-2-Clause-Patent
// Nintendo Switch (OLED, T210B01) battery for iPadOS: MAX17050 fuel gauge (0x36) and BQ24193 charger (0x6B) on I2C1,
// published as an IOPMPowerSource, and the charger's settings (SwitchChargePolicy.h has every limit and its source).
// Re-read when one of them interrupts (the charger's and gauge's TegraGPIO pins, see kAlerts) or the BM92T36 USB-PD
// controller changes something (SwitchPD, which owns that chip and its ALERT and tells us about every event), plus a
// slow re-read that bounds the staleness of what has no interrupt (voltage, current) and carries the stats line. The
// battery temperature has one: the gauge's temperature window, a degree either way of the last reading for
// thermalmonitord's battery sensors (switch_battery_temperature), narrowed to the charge zone's edges.
// There is no polled mode: alerts that cannot be attached, keep firing, or never reach the CPU panic.
//
// powerd (iOS 17) matches IOServiceMatching("IOPMPowerSource") with a first-match
// notification, then re-reads all properties whenever the source sends
// kIOPMMessageBatteryStatusHasChanged (IOPMPowerSource::updateStatus does). It requires
// BatteryInstalled == true plus CurrentCapacity and MaxCapacity, and shows
// round(100 * CurrentCapacity / MaxCapacity), so CurrentCapacity is a percentage and
// MaxCapacity is 100, as on Apple's own gauges; raw mAh go in AppleRaw*Capacity.
// The service registers only after the chips identify and a first reading is published.
//
// Conversions (hekate bdk/power/max17050.c, effective Rsense 10 mOhm: 5 mOhm, CGAIN 2):
// capacity LSB 0.5 mAh, current LSB 156.25 uA (positive = charging), VCELL LSB 0.625 mV
// (bits 15:3), TEMP s16 1/256 C, RepSOC 1/256 %, Cycles 1 % of a cycle (datasheet).
//
// The charger. At start one line says what an earlier boot or the bootloader left (REG00-REG0A; REG09 bit 7 set =
// default mode: the chip's I2C watchdog expired, or it never had a host write, and every register is at its reset
// value). Then, as Horizon (Atmosphere Bq24193Driver::InitializeSession) and switchroot (bq2419x_charger_init) do: the
// I2C watchdog off (so nothing reverts to defaults behind us, and nothing has to be kicked), the safety timer off,
// pre-charge/termination 128 mA with termination on, SYS_MIN 3000 mV, VINDPM 4360 mV, TREG 120 C, no IR compensation,
// and the charge current/voltage of the battery's temperature zone. The zone comes from the MAX17050's TEMP and changes
// by interrupt: TALRT_Th is set to the zone's window (charge::temperatureWindow), so the gauge's ALRT fires when the
// battery leaves it. The input current limit (REG00 IINLIM): a USB-PD contract SwitchPD requested and verified gives
// it; a USB-PD source without one (the controller's own contract while the request runs, or a failed request) and a
// contract withdrawn while the cable stays get 500 mA (Horizon boot: before anything is known about the source); a
// source that is not USB-PD keeps the chip's own detection on plug-in ([TI] 8.3.1.3), raised to 500 mA if it is lower,
// and Hi-Z is cleared. A start-up battery temperature that is not plausible leaves the charger as found (one line).
// Every charger interrupt reads REG09 twice (the chip sends no INT for new faults until it is read) and checks the
// settings: a chip that lost them (power-on reset) is set again with one line. A setup the chip does not take is put
// back as found with one line, and the driver then only reads.
// REG01 has one writer, under one lock: CHG_CONFIG is OTG while this boot's boost runs (switch_battery_set_otg_boost,
// for SwitchPD's VBUS to a sink-only partner), off in a no-charge temperature zone, charge otherwise; BOOST_LIM 1.3 A
// with the boost (bq2419x_vbus_enable). An earlier boot that rebooted with its OTG boost on is put back at start.
// The gauge's alert is armed as Linux max17042_battery.c does (max17042_set_soc_threshold, max17042_thread_handler):
// CONFIG.Aen, an SOC window of +-1 % around the current RepSOC, re-centred on every reading, STATUS alert bits cleared
// (POR, BST kept); voltage window wide open (maxim,volt-alrt-threshold 0xff00); temperature window about +-1 C around
// the reading (switch_max17050_talrt) intersected with the zone's (charge::intersectWindows) while managed.
// A stats line (os_log, not the console) at every change of contract, input limit or zone and every 4 minutes.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <os/log.h>
#include "../TegraI2C/TegraI2C.h"
#include "../TegraGPIO/TegraGPIO.h"
#include "../SwitchUSB/SwitchPD.h"
#include "SwitchBattery.h"
#include "SwitchChargePolicy.h"
#include "../SwitchPower/SwitchPower.h"
#include "../SwitchPower/SwitchSensors.h"
#include "../SwitchSuspend/SwitchWake.h"

namespace {
namespace bq = charge::bq;
constexpr unsigned kBus = 1;
constexpr uint32_t kIdentifyRetryMS = 30000;  // until the chips answer on I2C1
constexpr uint32_t kRefreshMS = 120000;       // re-read for the values without an interrupt
#if DEVELOPMENT || DEBUG
constexpr unsigned kStatsEvery = 2;  // re-reads per periodic stats line (4 minutes)
#endif
constexpr uint32_t kSettleMS = 250;   // after an interrupt: coalesce the chips' alerts, let them settle
constexpr unsigned kAlertBurst = 20;  // interrupts per second from one line that mean it is stuck
constexpr unsigned kLostLimit = 3;    // settings lost this many readings in a row: stop managing
constexpr uint8_t kGauge = 0x36, kCharger = 0x6b;

// MAX17050 registers.
enum : uint8_t {
    Status = 0x00,
    VAlrtTh = 0x01,
    TAlrtTh = 0x02,
    SAlrtTh = 0x03,
    Config = 0x1d,
    RepCap = 0x05,
    RepSOC = 0x06,
    Temp = 0x08,
    VCell = 0x09,
    Current = 0x0a,
    AvgCurrent = 0x0b,
    QH = 0x4d,
    FullCap = 0x10,
    Cycles = 0x17,
    DesignCap = 0x18,
    DevName = 0x21,
};

constexpr unsigned kStatusKeep = (1u << 1) | (1u << 3);  // POR, BST; the rest are alert flags
constexpr uint16_t kConfigAen = 1u << 2;

// Interrupt lines, all on TegraGPIO (OLED DTB, decompiled: i2c@7000c000 children):
//   BQ24193 INT   PZ0  interrupts = <0xc8 0x00>; a 256 us active-low pulse per event (datasheet): falling edge.
//   MAX17050 ALRT PY0  interrupts = <0xc0 0x00>; open drain, low while a STATUS alert flag is set: falling edge,
//                      and every reading clears the flags, so the next alert is a new edge.
//   BM92T36 ALERT PK4  SwitchPD's (level low, released by its ALERT_STATUS read); its events reach pdChanged().
struct Alert {
    unsigned gpio, trigger;
    const char *name;
};

constexpr Alert kAlerts[] = {
    {TEGRA_GPIO_PZ0, TEGRA_GPIO_EDGE_FALLING, "BQ24193 INT (PZ0)"},
    {TEGRA_GPIO_PY0, TEGRA_GPIO_EDGE_FALLING, "MAX17050 ALRT (PY0)"},
};
constexpr unsigned kAlertCount = sizeof(kAlerts) / sizeof(kAlerts[0]);

// What SwitchPD last said, packed so its work loop can hand it to ours in one atomic store.
struct PdSnapshot {
    bool plugged, pdSource;
    unsigned millivolts, inputLimit;
};

const char *inputSource(bool boosting, bool good, const PdSnapshot &pd, unsigned vbus)
{
    if (boosting)
        return "our OTG boost";
    if (!good)
        return "none";
    if (pd.inputLimit)
        return "USB-PD";
    if (pd.pdSource)
        return "USB-PD (controller's contract)";
    if (vbus == 1)
        return "USB host";
    return vbus == 2 ? "adapter" : "unknown";
}

unsigned flag(unsigned value, unsigned mask)
{
    return (value & mask) ? 1 : 0;
}

uint64_t packPd(const SwitchPDState *s)
{
    return 1ULL | static_cast<uint64_t>(s->plugged) << 1 | static_cast<uint64_t>(s->pdSource) << 2 |
           static_cast<uint64_t>(s->contractMillivolts) << 16 | static_cast<uint64_t>(s->inputLimitMilliamps) << 32;
}

PdSnapshot unpackPd(uint64_t w)
{
    // Before SwitchPD's first word (or without the controller): a cable might be in; no contract.
    if (!(w & 1))
        return PdSnapshot{true, false, 0, 0};
    return PdSnapshot{(w >> 1 & 1) != 0, (w >> 2 & 1) != 0, static_cast<unsigned>(w >> 16 & 0xffff),
                      static_cast<unsigned>(w >> 32 & 0xffff)};
}

struct Reading {
    unsigned percent, rawPercent, millivolts, cycles;
    int rawCapacity, fullCapacity, designCapacity, milliamps, avgMilliamps, centiCelsius, milliCelsius;
    bool external, charging, full;
    uint8_t charger[9];  // BQ24193 REG00-REG08 (one multi-read, [TI] 8.3.6.5.2)
    uint8_t fault;       // REG09: the latched read and the current one, OR'd ([TI] 8.3.4.3)
};

int gaugeWord(uint8_t reg, uint16_t *value)
{
    uint8_t b[2] = {};
    const int err = tegra_i2c_read(kBus, kGauge, reg, b, sizeof(b));
    *value = static_cast<uint16_t>(b[0] | static_cast<unsigned>(b[1]) << 8);
    return err;
}

int gaugeWrite(uint8_t reg, uint16_t value)
{
    const uint8_t b[2] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
    return tegra_i2c_write(kBus, kGauge, reg, b, sizeof(b));
}

// Alert on the next 1 % step of RepSOC either way and when the temperature leaves `temperatureWindow` (about a degree
// around the reading for thermalmonitord, switch_max17050_talrt; within the charge zone's edges while the charger is
// managed), the voltage window wide open (the DT's maxim,volt-alrt-threshold 0xff00), then clear the alert flags so
// ALRT releases.
int armGauge(unsigned percent, uint16_t temperatureWindow)
{
    uint16_t config = 0;
    uint16_t status = 0;
    int err = gaugeWrite(SAlrtTh, static_cast<uint16_t>((percent + 1) << 8 | (percent > 1 ? percent - 1 : 0)));
    if (!err)
        err = gaugeWrite(VAlrtTh, 0xff00);
    if (!err)
        err = gaugeWrite(TAlrtTh, temperatureWindow);
    if (!err)
        err = gaugeWord(Config, &config);
    if (!err && !(config & kConfigAen))
        err = gaugeWrite(Config, config | kConfigAen);
    if (!err)
        err = gaugeWord(Status, &status);
    if (!err && (status & ~kStatusKeep))
        err = gaugeWrite(Status, status & kStatusKeep);
    return err;
}

// REG09's faults as every reader of it saw them (readFaults: refresh, the as-found line, switch_battery_describe_otg),
// taken by the next reading: reading REG09 clears what latched, so no reader may lose them for the others.
uint8_t gFaultsSeen;

// REG09 twice: the first read returns (and clears) what latched since the last one, the second the state now; a fault
// is reported again only after it was read ([TI] 8.3.4.3).
int readFaults(uint8_t *latched, uint8_t *now)
{
    int err = tegra_i2c_read(kBus, kCharger, bq::Fault, latched, 1);
    if (!err)
        err = tegra_i2c_read(kBus, kCharger, bq::Fault, now, 1);
    if (!err)
        __atomic_fetch_or(&gFaultsSeen, static_cast<uint8_t>(*latched | *now), __ATOMIC_ACQ_REL);
    return err;
}

// REG00-REG08 in one read, then REG09 twice.
int readCharger(uint8_t regs[9], uint8_t *latched, uint8_t *now)
{
    int err = tegra_i2c_read(kBus, kCharger, bq::InputSource, regs, 9);
    if (!err)
        err = readFaults(latched, now);
    return err;
}

int chargerUpdate(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t current = 0;
    int err = tegra_i2c_read(kBus, kCharger, reg, &current, 1);
    const unsigned bits = mask;
    const uint8_t next = static_cast<uint8_t>((current & ~bits) | (value & bits));
    if (!err && next != current)
        err = tegra_i2c_write(kBus, kCharger, reg, &next, 1);
    return err;
}

// The settings of a decision, watchdog first (the first host write starts it: [TI] 8.4.1), then less charge before
// more. REG00's IINLIM and Hi-Z are applyInput()'s, REG01 is the arbiter's.
int writeCharger(const charge::Decision &d)
{
    const charge::ChargerSetting s = charge::chargerSetting(d);
    int err = chargerUpdate(bq::TerminationTimer, charge::kReg05Mask, s.reg05);
    if (!err)
        err = chargerUpdate(bq::ChargeCurrent, bq::kIchgMask | bq::kForce20Pct, s.reg02);
    if (!err)
        err = chargerUpdate(bq::PreChargeTermination, 0xff, s.reg03);
    if (!err)
        err = chargerUpdate(bq::ChargeVoltage, bq::kVregMask, s.reg04);
    if (!err)
        err = chargerUpdate(bq::IrThermal, charge::kReg06Mask, s.reg06);
    if (!err)
        err = chargerUpdate(bq::InputSource, bq::kVindpmMask, charge::kReg00Vindpm);
    return err;
}

bool chargerMatches(const uint8_t regs[9], const charge::Decision &d)
{
    const charge::ChargerSetting s = charge::chargerSetting(d);
    return (regs[0] & bq::kVindpmMask) == charge::kReg00Vindpm &&
           (regs[2] & (bq::kIchgMask | bq::kForce20Pct)) == s.reg02 && regs[3] == s.reg03 &&
           (regs[4] & bq::kVregMask) == s.reg04 && (regs[5] & charge::kReg05Mask) == s.reg05 &&
           (regs[6] & charge::kReg06Mask) == s.reg06;
}

void setNumber(OSDictionary *dict, const char *key, int64_t value)
{
    if (const OSNumber *n = OSNumber::withNumber(static_cast<unsigned long long>(value), 32)) {
        dict->setObject(key, n);
        n->release();
    }
}

// ChargerData ([IOS-TLC]: PowerUI and PowerLog read NotChargingReason from it): what the charger is set to and why it
// does not charge, as Apple's battery services publish it.
OSDictionary *chargerData(const charge::Decision &d, uint8_t reg00, bool charging)
{
    OSDictionary *dict = OSDictionary::withCapacity(5);
    if (!dict)
        return nullptr;
    setNumber(dict, "NotChargingReason", d.notChargingReason);
    setNumber(dict, "ChargingCurrent", charging ? d.chargeCurrentMA : 0);
    setNumber(dict, "ChargingVoltage", d.chargeVoltageMV);
    setNumber(dict, "VacVoltageLimit", d.chargeVoltageMV);
    setNumber(dict, "InputCurrentLimit", bq::decodeInputLimit(reg00));
    return dict;
}

// Adapter details for powerd/IOPSCopyExternalPowerAdapterDetails: the published USB-PD contract (SwitchPD) at its
// input limit; otherwise the BQ24193 input current limit at 5 V (a USB-PD source without our contract, a USB host
// port or a charging port, from REG08 VBUS_STAT).
OSDictionary *adapterDetails(bool external, uint8_t reg00, uint8_t reg08, const PdSnapshot &pd)
{
    OSDictionary *dict = OSDictionary::withCapacity(5);
    if (!dict)
        return nullptr;
    if (!external) {
        setNumber(dict, kIOPMPSAdapterDetailsFamilyKey, kIOPSFamilyCodeDisconnected);
        return dict;
    }
    int millivolts = 5000;
    const int milliamps = static_cast<int>(bq::decodeInputLimit(reg00));
    int family = bq::vbusStat(reg08) == 1 ? kIOPSFamilyCodeUSBHost : kIOPSFamilyCodeUSBChargingPort;
    const char *description = "usb charger";
    if (pd.inputLimit) {
        millivolts = static_cast<int>(pd.millivolts);
        family = kIOPSFamilyCodeUSBCPD;
        description = "pd charger";
    } else if (pd.pdSource) {
        family = kIOPSFamilyCodeUSBCPD;
        description = "pd charger";
    }
    setNumber(dict, kIOPMPSAdapterDetailsFamilyKey, family);
    setNumber(dict, kIOPMPSAdapterDetailsWattsKey, static_cast<int64_t>(millivolts) * milliamps / 1000000);
    setNumber(dict, kIOPMPSAdapterDetailsVoltage, millivolts);
    setNumber(dict, kIOPMPSAdapterDetailsAmperageKey, milliamps);
    setNumber(dict, "PMUConfiguration", milliamps);  // [IOS-CLTM]: the input current limit thermalmonitord reads
    dict->setObject("IsWireless", kOSBooleanFalse);
    if (const OSString *text = OSString::withCString(description)) {
        dict->setObject(kIOPMPSAdapterDetailsDescriptionKey, text);
        text->release();
    }
    return dict;
}

// REG01's writers (start-up, the temperature zone, switch_battery_set_otg_boost) under one lock, created on first use.
IOLock *gBoostLock;
bool gBoosting;              // this boot's boost is on
bool gChargeAllowed = true;  // the decision (charge::decide: temperature zone and iPadOS's requests) charges
bool gManaged;               // the charger is ours (SYS_MIN too)

IOLock *boostLock()
{
    IOLock *lock = __atomic_load_n(&gBoostLock, __ATOMIC_ACQUIRE);
    if (lock)
        return lock;
    IOLock *fresh = IOLockAlloc();
    IOLock *none = nullptr;
    if (!fresh)
        return nullptr;
    if (__atomic_compare_exchange_n(&gBoostLock, &none, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return fresh;
    IOLockFree(fresh);
    return none;
}

// The BQ24193's I2C watchdog (REG05 5:4), its one owner (boost lock held). A host write starts it (40 s from reset,
// [TI] 8.4.1) and its expiry resets every register, so it is off whenever that would undo something of ours: always
// while the charger is managed (writeCharger, as Horizon and switchroot run it: InitializeSession watchdog 0,
// ti,watchdog-timeout <0>), and, not managed, while the boost runs or charging is off by temperature (switchroot
// ti,no-otg-watchdog: no watchdog in OTG). Not managed, it goes back to what it was once neither holds (the boost
// ended), so an unmanaged charger is otherwise as it was found.
uint8_t gWatchdogSaved = 0xff;  // REG05 watchdog bits from before we turned it off (not managed); 0xff: none saved

int watchdogLocked()
{
    if (gManaged)
        return 0;  // off with the rest of the settings (writeCharger); nothing to restore
    uint8_t reg = 0;
    int err = tegra_i2c_read(kBus, kCharger, bq::TerminationTimer, &reg, 1);
    if (err)
        return err;
    uint8_t next = reg;
    if (gBoosting || !gChargeAllowed) {
        if ((reg & bq::kWatchdogMask) && gWatchdogSaved == 0xff)
            gWatchdogSaved = reg & bq::kWatchdogMask;
        next = static_cast<uint8_t>(reg & ~bq::kWatchdogMask);
    } else if (gWatchdogSaved != 0xff) {
        next = static_cast<uint8_t>((reg & ~bq::kWatchdogMask) | gWatchdogSaved);
    }
    if (next != reg)
        err = tegra_i2c_write(kBus, kCharger, bq::TerminationTimer, &next, 1);
    if (!err && !gBoosting && gChargeAllowed)
        gWatchdogSaved = 0xff;
    return err;
}

// REG01 from the state above (boost lock held): CHG_CONFIG OTG with the boost, else charge or off by the zone;
// BOOST_LIM 1.3 A with the boost; SYS_MIN 3000 mV once managed; the self-clearing reset bits written 0. The watchdog
// first (watchdogLocked): off before a REG01 its expiry would undo, back (not managed) after the boost ends.
int reg01Locked(uint8_t *was, uint8_t *now)
{
    uint8_t reg = 0;
    int err = watchdogLocked();
    if (!err)
        err = tegra_i2c_read(kBus, kCharger, bq::PowerOnConfig, &reg, 1);
    if (err)
        return err;
    uint8_t next = static_cast<uint8_t>(reg & ~(bq::kChgConfigMask | bq::kWatchdogReset | 0x80));
    unsigned mode = gChargeAllowed ? bq::kChgConfigCharge : bq::kChgConfigOff;
    if (gBoosting)
        mode = bq::kChgConfigOtg;
    next |= mode;
    if (gBoosting)
        next |= bq::kBoostLim1300;
    if (gManaged)
        next = static_cast<uint8_t>((next & ~bq::kSysMinMask) | charge::kReg01SysMin);
    if (was)
        *was = reg;
    if (now)
        *now = next;
    return next == reg ? 0 : tegra_i2c_write(kBus, kCharger, bq::PowerOnConfig, &next, 1);
}

// REG01 after a change of `gChargeAllowed`/`gManaged` (nullptr: keep them), or as a check.
int applyReg01(const bool *chargeAllowed, const bool *managed, uint8_t *was, uint8_t *now, bool *boosting)
{
    IOLock *lock = boostLock();
    if (!lock)
        return -1;
    IOLockLock(lock);
    if (chargeAllowed)
        gChargeAllowed = *chargeAllowed;
    if (managed)
        gManaged = *managed;
    const int err = reg01Locked(was, now);
    if (boosting)
        *boosting = gBoosting;
    IOLockUnlock(lock);
    return err;
}

// Start-up REG01: CHG_CONFIG to charge (or off, in a no-charge zone) unless this boot's own boost is on. An earlier
// boot that rebooted with its OTG boost on (or anything else in REG01) must not keep the battery from charging. One
// line when it changes CHG_CONFIG.
int startMode(const bool *chargeAllowed, const bool *managed)
{
    uint8_t was = 0;
    uint8_t now = 0;
    const int err = applyReg01(chargeAllowed, managed, &was, &now, nullptr);
    static const char *const kModes[] = {"off", "charge", "OTG", "OTG"};
    if (err) {
        IOLog("SwitchBattery: charge mode check failed (I2C %d)\n", err);
    } else if ((was & bq::kChgConfigMask) != (now & bq::kChgConfigMask)) {
        IOLog("SwitchBattery: charger was left with CHG_CONFIG %u (REG01 %#x), now %s (REG01 %#x)\n",
              (was & bq::kChgConfigMask) >> 4, was, kModes[(now & bq::kChgConfigMask) >> 4], now);
    }
    return err;
}

// The battery temperature for thermalmonitord's sensors (switch_battery_temperature): written by the work loop only,
// generation (odd while writing), value, time, generation.
uint32_t gTempGen;
int gTempMilliC;
uint64_t gTempAt;  // 0: no reading (none yet, or the last read failed)

void storeTemperature(bool good, int milliC)
{
    const uint64_t before = __atomic_load_n(&gTempAt, __ATOMIC_RELAXED);
    __atomic_store_n(&gTempGen, gTempGen + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&gTempMilliC, milliC, __ATOMIC_RELAXED);
    __atomic_store_n(&gTempAt, good ? mach_absolute_time() : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&gTempGen, gTempGen + 1, __ATOMIC_RELEASE);
    if ((before == 0u) != !good)
        switch_sensors_note();
}

void formatCentiC(char *out, size_t size, int centi)
{
    const unsigned a = static_cast<unsigned>(centi < 0 ? -centi : centi);
    snprintf(out, size, "%s%u.%02u", centi < 0 ? "-" : "", a / 100, a % 100);
}

// What REG08 says: external power (not our boost), charging, done.
void statusFields(Reading *r)
{
    const uint8_t status = r->charger[bq::SystemStatus];
    const unsigned vbus = bq::vbusStat(status);
    const unsigned chrg = bq::chargeStat(status);
    // VBUS 3 = OTG: we power the partner; and while we source the port (the boost on), what the charger sees is ours.
    r->external = ((status & bq::kPowerGood) != 0) && vbus != 3 && !__atomic_load_n(&gBoosting, __ATOMIC_RELAXED);
    r->charging = r->external && (chrg == 1 || chrg == 2);
    r->full = r->external && chrg == 3;
}

int readAll(Reading *r)
{
    uint16_t repCap = 0;
    uint16_t repSoc = 0;
    uint16_t temp = 0;
    uint16_t vcell = 0;
    uint16_t current = 0;
    uint16_t avgCurrent = 0;
    uint16_t fullCap = 0;
    uint16_t cycles = 0;
    uint16_t designCap = 0;
    uint8_t latched = 0;
    uint8_t now = 0;
    int err = gaugeWord(RepCap, &repCap);
    if (!err)
        err = gaugeWord(RepSOC, &repSoc);
    if (!err)
        err = gaugeWord(Temp, &temp);
    if (!err)
        err = gaugeWord(VCell, &vcell);
    if (!err)
        err = gaugeWord(Current, &current);
    if (!err)
        err = gaugeWord(AvgCurrent, &avgCurrent);
    if (!err)
        err = gaugeWord(FullCap, &fullCap);
    if (!err)
        err = gaugeWord(Cycles, &cycles);
    if (!err)
        err = gaugeWord(DesignCap, &designCap);
    if (!err)
        err = readCharger(r->charger, &latched, &now);
    if (err)
        return err;
    r->fault = __atomic_exchange_n(&gFaultsSeen, 0, __ATOMIC_ACQ_REL);  // ours and the other readers' since the last

    statusFields(r);
    r->rawPercent = repSoc >> 8;
    r->percent = r->full ? 100 : (repSoc + 128u) >> 8;
    if (r->percent > 100)
        r->percent = 100;
    r->rawCapacity = repCap / 2;
    r->fullCapacity = fullCap / 2;
    r->designCapacity = designCap / 2;
    r->cycles = cycles / 100u;
    r->millivolts = (vcell >> 3) * 625u / 1000;
    r->milliamps = static_cast<int16_t>(current) * 15625 / 100000;
    r->avgMilliamps = static_cast<int16_t>(avgCurrent) * 15625 / 100000;
    r->centiCelsius = static_cast<int16_t>(temp) * 100 / 256;
    r->milliCelsius = switch_max17050_millic(temp);
    return 0;
}
}  // namespace

// switch_battery_avg_ma: the last reading's AvgCurrent (mA) and whether external power was in.
static int gBatteryAvgMa = INT32_MIN;
static bool gBatteryExternal;

extern "C" bool native_aks_device_digest(const char *label, uint8_t out[32]);

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage (its kalloc type view)
class SwitchBattery : public IOPMPowerSource {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchBattery);
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *timer = nullptr;
    IOInterruptEventSource *alerts[kAlertCount] = {};
    bool published = false;
    bool serialPublished = false;
    bool reportedFailure = false;
    unsigned alertCount[kAlertCount] = {};
    uint64_t alertWindowStart[kAlertCount] = {};
    // The charger (file comment).
    bool managed = false;
    bool zoneControl = false;  // the decision switches charging on/off (also when not managed: never more charge)
    charge::Zone zone = charge::Zone::Cold;
    // The one owner of what the charger does: the zone and iPadOS's requests (setProperties) through charge::decide;
    // `decision` is what the chip was last set to. Both only on the work loop.
    charge::Requests requests = {};
    charge::Decision decision = charge::decide(charge::Zone::Cold, charge::Requests{});
    bool wasExternal = false;  // power in (not ours) as of the last reading: an attach is a wake source
    int lastCentiCelsius = 0;  // the last reading's battery temperature
    bool appliedPd = false;    // an input limit from a USB-PD contract was programmed since the cable came
    bool boosting = false;     // this boot's OTG boost, as of the last reading
    bool statsDue = true;
    unsigned refreshes = 0, lostInARow = 0;
    uint64_t pdWord = 0;  // SwitchPD's last state (packPd), written on its work loop

    static void timerFired(OSObject *owner, IOTimerEventSource *sender);
    static void alertFired(OSObject *owner, IOInterruptEventSource *sender, int count);
    static void pdChanged(void *context, const SwitchPDState *state);
    void attachAlerts();
    void publishSerial();
    bool identify();
    bool refresh();
    static bool reportAsFound(uint8_t found[11]);
    bool setUpCharger(const uint8_t found[11]);
    void manage(Reading &r, const PdSnapshot &pd);
    void apply(const charge::Decision &next, const char *why, int centiCelsius);
    void followZone(const Reading &r);
    static IOReturn requestAction(OSObject *owner, void *dict, void *, void *, void *);
    IOReturn request(const OSDictionary *dict);
    void applyInput(Reading &r, const PdSnapshot &pd);
    void stats(const Reading &r, const PdSnapshot &pd);
    void setKey(const char *key, OSObject *value);
    void setKeyNumber(const char *key, int64_t value);

public:
    bool start(IOService *provider) override;
    void free() override;
    IOReturn setProperties(OSObject *properties) override;

    IOWorkLoop *getWorkLoop() const override
    {
        return loop;
    }

    void sleep(bool entering);
};

OSDefineMetaClassAndStructors(SwitchBattery, IOPMPowerSource);

void SwitchBattery::setKey(const char *key, OSObject *value)
{
    if (const OSSymbol *symbol = OSSymbol::withCString(key)) {
        setPSProperty(symbol, value);
        symbol->release();
    }
}

// The pack has no readable serial (the MAX17050 keeps none) and powerd's battery health needs one: 17 characters of
// Apple's serial alphabet, derived from this console's device key (AppleKeyStore), so the same on every boot and
// different on every console. Published once AppleKeyStore has started.
void SwitchBattery::publishSerial()
{
    static const char kAlphabet[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ";
    uint8_t digest[32];
    if (serialPublished || !native_aks_device_digest("Switch battery serial", digest))
        return;
    char serial[18];
    for (size_t i = 0; i < 17; ++i)
        serial[i] = kAlphabet[digest[i] % (sizeof(kAlphabet) - 1)];
    serial[17] = '\0';
    if (OSString *value = OSString::withCString(serial)) {
        setKey(kIOPMPSSerialKey, value);
        value->release();
        serialPublished = true;
    }
}

void SwitchBattery::setKeyNumber(const char *key, int64_t value)
{
    if (OSNumber *n = OSNumber::withNumber(static_cast<unsigned long long>(value), 32)) {
        setKey(key, n);
        n->release();
    }
}

bool SwitchBattery::start(IOService *provider)
{
    if (!IOPMPowerSource::start(provider))
        return false;
    loop = IOWorkLoop::workLoop();
    timer = loop ? IOTimerEventSource::timerEventSource(this, &SwitchBattery::timerFired) : nullptr;
    if (!timer || loop->addEventSource(timer) != kIOReturnSuccess) {
        IOLog("SwitchBattery: work loop setup failed\n");
        return false;
    }
    setName("SwitchBattery");
    attachAlerts();
    timer->setTimeoutMS(1);
    // The PD controller's events (a plug, a contract): settle and re-read, as for the other two lines.
    if (switch_pd_add_listener(&SwitchBattery::pdChanged, this))
        IOLog("SwitchBattery: no PD controller events\n");
    return true;
}

void SwitchBattery::free()
{
    for (auto *&alert : alerts) {
        if (!alert)
            continue;
        alert->disable();
        if (loop)
            loop->removeEventSource(alert);
        OSSafeReleaseNULL(alert);
    }
    if (timer) {
        timer->cancelTimeout();
        if (loop)
            loop->removeEventSource(timer);
        timer->release();
    }
    OSSafeReleaseNULL(loop);
    IOPMPowerSource::free();
}

bool SwitchBattery::identify()
{
    uint8_t name[2] = {};
    uint8_t vendor = 0;
    int err = tegra_i2c_init(kBus);
    if (!err)
        err = tegra_i2c_read(kBus, kGauge, DevName, name, sizeof(name));
    if (!err)
        err = tegra_i2c_read(kBus, kCharger, bq::VendorPart, &vendor, 1);
    if (!err && (name[0] | static_cast<unsigned>(name[1]) << 8) == 0x00ac && vendor == 0x2f) {
        uint8_t found[11] = {};
        const bool foundRead = reportAsFound(found);
        managed = foundRead && setUpCharger(found);  // no snapshot to fall back to: read only
        if (!managed)
            (void)startMode(nullptr, nullptr);
        return true;
    }
    if (!reportedFailure) {
        IOLog("SwitchBattery: MAX17050/BQ24193 not found on I2C1 (err %d, dev 0x%02x%02x, vendor 0x%02x)\n", err,
              name[1], name[0], vendor);
        reportedFailure = true;
    }
    return false;
}

// One line with the charger as an earlier boot, the bootloader or the chip's own reset left it, before any write.
bool SwitchBattery::reportAsFound(uint8_t found[11])
{
    uint8_t latched = 0;
    uint8_t now = 0;
    const int err = readCharger(found, &latched, &now);
    found[9] = now;
    found[10] = latched;
    if (err) {
        IOLog("SwitchBattery: BQ24193 as found: read failed (I2C %d): charger left alone, read only\n", err);
        return false;
    }
    static const char *const kVbus[] = {"none", "USB host", "adapter", "OTG"};
    static const char *const kCharge[] = {"not charging", "pre-charge", "fast charge", "done"};
    IOLog("SwitchBattery: BQ24193 as found: REG00-09 %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x/%02x: input "
          "limit %u mA%s, VINDPM %u mV, ICHG %u mA%s, VREG %u mV, watchdog %u s, safety timer %s, VBUS %s%s%s, %s, %s "
          "mode\n",
          found[0], found[1], found[2], found[3], found[4], found[5], found[6], found[7], found[8], latched, now,
          bq::decodeInputLimit(found[0]), (found[0] & bq::kHiZ) ? " (Hi-Z)" : "", bq::decodeInputVoltage(found[0]),
          bq::decodeChargeCurrent(found[2]), (found[2] & bq::kForce20Pct) ? " (20 %)" : "",
          bq::decodeChargeVoltage(found[4]), bq::watchdogSeconds(found[5]), (found[5] & bq::kEnTimer) ? "on" : "off",
          kVbus[bq::vbusStat(found[8])], (found[8] & bq::kPowerGood) ? ", power good" : "",
          (found[8] & bq::kDpm) ? ", in DPM" : "", kCharge[bq::chargeStat(found[8])],
          ((static_cast<unsigned>(latched) | now) & bq::kFaultWatchdog) ? "default" : "host");
    return true;
}

// The settings (file comment); a chip that does not take them is put back as found, with one line.
bool SwitchBattery::setUpCharger(const uint8_t found[11])
{
    uint16_t temp = 0;
    int err = gaugeWord(Temp, &temp);
    const int centi = static_cast<int16_t>(temp) * 100 / 256;
    char celsius[16];
    formatCentiC(celsius, sizeof(celsius), centi);
    if (!err && (!temp || centi < charge::kPlausibleMinCentiC || centi > charge::kPlausibleMaxCentiC)) {
        IOLog("SwitchBattery: battery temperature %s C (TEMP 0x%04x) is not plausible: charger left as found, read "
              "only\n",
              celsius, temp);
        return false;
    }
    zone = charge::nextZone(charge::Zone::Cold, centi);  // from no charge: the entry margins apply
    zoneControl = true;
    decision = charge::decide(zone, requests);
    if (!err)
        err = writeCharger(decision);
    const bool charges = decision.charge;
    const bool yes = true;
    if (!err)
        err = startMode(&charges, &yes);
    uint8_t now[9] = {};
    uint8_t latched = 0;
    uint8_t current = 0;
    if (!err)
        err = readCharger(now, &latched, &current);
    if (!err && chargerMatches(now, decision)) {
        __atomic_store_n(&gFaultsSeen, 0, __ATOMIC_RELEASE);  // the as-found faults (default mode) are not news
        IOLog("SwitchBattery: charger set: VINDPM %u mV, ICHG %u mA, VREG %u mV, pre-charge/termination %u/%u mA, "
              "watchdog off, safety timer off, battery %s C (%s%s)\n",
              bq::decodeInputVoltage(now[0]), bq::decodeChargeCurrent(now[2]), bq::decodeChargeVoltage(now[4]),
              charge::kPreChargeMA, charge::kTerminationMA, celsius, charge::zoneName(zone),
              charge::zoneCharges(zone) ? "" : ": charging off");
        return true;
    }
    // Back as found (REG02-REG06 and REG00's VINDPM; REG00's IINLIM was not touched); REG01 still charges, but only
    // within the temperature zones that charge (zoneControl: the one thing kept, as it only ever stops charging).
    for (uint8_t reg = bq::ChargeCurrent; reg <= bq::IrThermal; ++reg) {
        (void)tegra_i2c_write(kBus, kCharger, reg, &found[reg], 1);
    }
    (void)chargerUpdate(bq::InputSource, bq::kVindpmMask, found[bq::InputSource]);
    const bool allowed = decision.charge;
    const bool no = false;
    (void)startMode(&allowed, &no);
    if (IOLock *lock = boostLock()) {  // SYS_MIN as found (REG01's other bits are the arbiter's)
        IOLockLock(lock);
        (void)chargerUpdate(bq::PowerOnConfig, bq::kSysMinMask, found[bq::PowerOnConfig]);
        IOLockUnlock(lock);
    }
    IOLog("SwitchBattery: charger setup did not take (I2C %d, REG00-06 %02x %02x %02x %02x %02x %02x %02x): left as "
          "found but for charging on/off by temperature\n",
          err, now[0], now[1], now[2], now[3], now[4], now[5], now[6]);
    return false;
}

bool SwitchBattery::refresh()
{
    Reading r = {};
    const int err = readAll(&r);
    if (err) {
        if (!reportedFailure)
            IOLog("SwitchBattery: read failed (%d), keeping last values\n", err);
        reportedFailure = true;
        storeTemperature(false, 0);
        return false;
    }
    reportedFailure = false;
    storeTemperature(true, r.milliCelsius);
    lastCentiCelsius = r.centiCelsius;
    const PdSnapshot pd = unpackPd(__atomic_load_n(&pdWord, __ATOMIC_ACQUIRE));
    if (managed) {
        manage(r, pd);
    } else if (zoneControl) {
        followZone(r);
    }
    // What the charger does now (manage may just have stopped or started it), for what is published below.
    if (zoneControl && !tegra_i2c_read(kBus, kCharger, bq::SystemStatus, &r.charger[bq::SystemStatus], 1)) {
        statusFields(&r);
    }
    // Power in that is not ours: SpringBoard lights the screen with the charging UI on this change ([IOS-SB]); with the
    // panel off it is also a wake source, so the wake guarantee lights it if iPadOS does not (SwitchWake.h).
    if (published && r.external && !wasExternal)
        switch_wake_event(SWITCH_WAKE_POWER_IN);
    wasExternal = r.external;
    const bool inflowOff = zoneControl && requests.inflowDisabled;  // [IOS-PD] DisableInflow: we report no AC
    setBatteryInstalled(true);
    setExternalConnected(r.external && !inflowOff);
    setExternalChargeCapable(r.external && !inflowOff);
    setIsCharging(r.charging && !inflowOff);
    setAtCriticalLevel(false);
    setCurrentCapacity(r.percent);
    setMaxCapacity(100);
    setVoltage(r.millivolts);
    setAmperage(r.avgMilliamps);
    __atomic_store_n(&gBatteryExternal, r.external, __ATOMIC_RELAXED);
    __atomic_store_n(&gBatteryAvgMa, r.avgMilliamps, __ATOMIC_RELAXED);
    setCycleCount(r.cycles);
    setKey(kIOPMFullyChargedKey, r.full ? kOSBooleanTrue : kOSBooleanFalse);
    setKey("AppleRawExternalConnected", (r.external || (inflowOff && pd.plugged)) ? kOSBooleanTrue : kOSBooleanFalse);
    // What iPadOS reads back about the charge ([IOS-PS], [IOS-TLC], [IOS-CLTM]); nothing claimed without the decision.
    const uint32_t notCharging = zoneControl && r.external && !r.charging ? decision.notChargingReason : 0;
    setKeyNumber("NotChargingReason", notCharging);
    setKeyNumber("ChargingOverride", zoneControl ? decision.chargingOverride : 0);
    setKeyNumber("AppleChargeRateLimitIndex", requests.thermalLimitIndex);
    charge::Decision shown = decision;
    shown.notChargingReason = notCharging;
    if (OSDictionary *data = chargerData(shown, r.charger[bq::InputSource], r.charging)) {
        setKey("ChargerData", data);
        data->release();
    }
    publishSerial();
    setKeyNumber("InstantAmperage", r.milliamps);
    setKeyNumber(kIOPMPSBatteryTemperatureKey, r.centiCelsius);
    setKeyNumber(kIOPMPSDesignCapacityKey, r.designCapacity);
    setKeyNumber("NominalChargeCapacity", r.fullCapacity);
    setKeyNumber("AppleRawCurrentCapacity", r.rawCapacity);
    setKeyNumber("AppleRawMaxCapacity", r.fullCapacity);
    if (OSDictionary *adapter =
            adapterDetails(r.external, r.charger[bq::InputSource], r.charger[bq::SystemStatus], pd)) {
        setKey(kIOPMPSAdapterDetailsKey, adapter);
        adapter->release();
    }
    updateStatus();
    // Acknowledge: re-centre the gauge's windows (releases ALRT).
    const uint16_t around = switch_max17050_talrt(r.milliCelsius);
    const uint16_t window =
        zoneControl ? charge::intersectWindows(charge::temperatureWindow(zone, r.centiCelsius), around) : around;
    if (armGauge(r.rawPercent, window) && !reportedFailure)
        IOLog("SwitchBattery: gauge alert setup failed\n");
    if (r.fault)
        statsDue = true;  // a charger fault is news
#if DEVELOPMENT || DEBUG
    if (statsDue || refreshes % kStatsEvery == 0) {
        stats(r, pd);
        statsDue = false;
    }
#else
    if (r.fault)
        stats(r, pd);
#endif
    return true;
}

// The charger, after every reading: settings still there, the zone, REG01, the input limit.
void SwitchBattery::manage(Reading &r, const PdSnapshot &pd)
{
    if ((r.fault & bq::kFaultWatchdog) || !chargerMatches(r.charger, decision)) {
        if (++lostInARow > kLostLimit) {
            const bool allowed = decision.charge;
            const bool no = false;
            (void)applyReg01(&allowed, &no, nullptr, nullptr, nullptr);
            managed = false;  // zoneControl stays: charging still stops outside 4-51 C
            IOLog("SwitchBattery: charger keeps losing its settings (REG00-06 %02x %02x %02x %02x %02x %02x %02x, "
                  "REG09 %#x): left alone from now but for charging on/off by temperature\n",
                  r.charger[0], r.charger[1], r.charger[2], r.charger[3], r.charger[4], r.charger[5], r.charger[6],
                  r.fault);
            return;
        }
        IOLog("SwitchBattery: charger settings lost (REG00-06 %02x %02x %02x %02x %02x %02x %02x, REG09 %#x): set "
              "again\n",
              r.charger[0], r.charger[1], r.charger[2], r.charger[3], r.charger[4], r.charger[5], r.charger[6],
              r.fault);
        (void)writeCharger(decision);
        statsDue = true;
    } else {
        lostInARow = 0;
    }
    const charge::Zone next = charge::nextZone(zone, r.centiCelsius);
    if (next != zone) {
        const charge::Zone from = zone;
        zone = next;
        char why[48];
        snprintf(why, sizeof(why), "%s -> %s", charge::zoneName(from), charge::zoneName(next));
        apply(charge::decide(zone, requests), why, r.centiCelsius);
    }
    uint8_t was = 0;
    uint8_t now = 0;
    const int err = applyReg01(nullptr, nullptr, &was, &now, &boosting);
    if (!err && was != now) {
        IOLog("SwitchBattery: REG01 was %#x, set %#x (CHG_CONFIG %u)\n", was, now, (now & bq::kChgConfigMask) >> 4);
    }
    applyInput(r, pd);
}

// The one place the charger's charge settings change (managed): REG01 by the arbiter, REG02/REG04, less charge first
// (charge::raises), one line. A failed write shows as lost settings at the next reading and is set again.
void SwitchBattery::apply(const charge::Decision &next, const char *why, int centiCelsius)
{
    const charge::Decision from = decision;
    const charge::ChargerSetting s = charge::chargerSetting(next);
    const bool charges = next.charge;
    int err = 0;
    decision = next;                    // what the chip must hold from now
    if (!charge::raises(from, next)) {  // less (or the same): stop or lower first
        err = applyReg01(&charges, nullptr, nullptr, nullptr, nullptr);
        if (!err)
            err = chargerUpdate(bq::ChargeCurrent, bq::kIchgMask | bq::kForce20Pct, s.reg02);
        if (!err)
            err = chargerUpdate(bq::ChargeVoltage, bq::kVregMask, s.reg04);
    } else {
        err = chargerUpdate(bq::ChargeVoltage, bq::kVregMask, s.reg04);
        if (!err)
            err = chargerUpdate(bq::ChargeCurrent, bq::kIchgMask | bq::kForce20Pct, s.reg02);
        if (!err)
            err = applyReg01(&charges, nullptr, nullptr, nullptr, nullptr);
    }
    char celsius[16];
    formatCentiC(celsius, sizeof(celsius), centiCelsius);
    IOLog("SwitchBattery: battery %s C: %s, %s (ICHG %u mA, VREG %u mV%s%s%s)%s\n", celsius, why,
          charges ? "charging" : "charging off", bq::decodeChargeCurrent(s.reg02), bq::decodeChargeVoltage(s.reg04),
          requests.osInhibit ? ", inhibited by powerd" : "", requests.predictiveInhibit ? ", held by PowerUI" : "",
          requests.inflowDisabled ? ", inflow off" : "", err ? " (I2C error: set again at the next reading)" : "");
    statsDue = true;
}

// Not managed (a setup that did not take, settings that kept getting lost) but the temperature known: only charging
// on/off by zone, through REG01; the rest of the charger stays as the chip has it.
void SwitchBattery::followZone(const Reading &r)
{
    const charge::Zone next = charge::nextZone(zone, r.centiCelsius);
    decision = charge::decide(next, requests);
    const bool charges = decision.charge;
    uint8_t was = 0;
    uint8_t now = 0;
    const int err = applyReg01(&charges, nullptr, &was, &now, nullptr);
    if (next == zone)
        return;
    char celsius[16];
    formatCentiC(celsius, sizeof(celsius), r.centiCelsius);
    IOLog("SwitchBattery: battery %s C: %s -> %s, %s (charger otherwise as found)%s\n", celsius, charge::zoneName(zone),
          charge::zoneName(next), charges ? "charging" : "charging off",
          err ? " (I2C error: set again at the next reading)" : "");
    zone = next;
    statsDue = true;
}

// REG00 IINLIM and Hi-Z (file comment). Raised one step at a time from 500 mA, 1 ms apart, lowered at once
// (bq2419x_configure_input_charging_current).
void SwitchBattery::applyInput(Reading &r, const PdSnapshot &pd)
{
    if (!pd.plugged)
        appliedPd = false;  // the next cable starts from the chip's own detection
    uint8_t reg00 = r.charger[bq::InputSource];
    const bool hiZ = (reg00 & bq::kHiZ) != 0;
    if (decision.hiZ) {  // [IOS-PD] DisableInflow: the input off, the system on the battery, until it is released
        if (!hiZ && !boosting) {
            // Under REG01's lock: a boost SwitchPD starts meanwhile is never put behind a disconnected input.
            IOLock *lock = boostLock();
            if (!lock)
                return;
            IOLockLock(lock);
            int err = 0;
            const uint8_t next = static_cast<uint8_t>(reg00 | bq::kHiZ);
            if (!gBoosting)
                err = tegra_i2c_write(kBus, kCharger, bq::InputSource, &next, 1);
            const bool written = !gBoosting && (err == 0);
            IOLockUnlock(lock);
            if (written)
                r.charger[bq::InputSource] = next;
            if (written || err) {
                IOLog("SwitchBattery: input off (Hi-Z) for powerd's DisableInflow%s\n", err ? " (I2C error)" : "");
            }
            statsDue = true;
        }
        return;
    }
    // Our boost, or no input to limit. Hi-Z is cleared even without power good: in Hi-Z (host mode) the chip neither
    // takes power nor runs its detection again ([TI] 8.3.1.3, 8.3.1.3.5).
    if (boosting || (!(r.charger[bq::SystemStatus] & bq::kPowerGood) && !hiZ))
        return;
    const unsigned found = bq::decodeInputLimit(reg00);
    unsigned target = found;
    const char *why = "the charger's own detection";
    if (pd.inputLimit) {
        target = pd.inputLimit;
        why = "USB-PD contract";
        appliedPd = true;
    } else if (appliedPd) {
        target = charge::kUnknownSourceMA;
        why = "USB-PD contract withdrawn";
    } else if (pd.pdSource) {
        target = charge::kUnknownSourceMA;  // the controller's own contract, of a voltage and current we do not know
        why = "USB-PD source without our contract";
    } else if (hiZ) {
        target = charge::kUnknownSourceMA;  // what was detected may be a previous source's
        why = "out of Hi-Z, unknown source";
    } else if (target < charge::kUnknownSourceMA) {
        target = charge::kUnknownSourceMA;
        why = "the charger's own detection, raised to 500 mA";
    }
    const uint8_t want = bq::encodeInputLimit(target);
    if ((reg00 & bq::kIinlimMask) == want && !hiZ)
        return;
    uint8_t step = reg00 & bq::kIinlimMask;
    const uint8_t floor = bq::encodeInputLimit(charge::kUnknownSourceMA);
    if (want > step && step < floor)
        step = floor;
    if (want < step)
        step = want;
    int err = 0;
    for (;;) {
        const uint8_t next = static_cast<uint8_t>((reg00 & ~(bq::kHiZ | bq::kIinlimMask)) | step);
        err = tegra_i2c_write(kBus, kCharger, bq::InputSource, &next, 1);
        if (err)
            break;
        reg00 = next;
        if (step >= want)
            break;
        IODelay(1000);  // BQ2419x_CHARGING_CURRENT_STEP_DELAY_US
        ++step;
    }
    r.charger[bq::InputSource] = reg00;
    // What the chip holds now, read back (the proof that the contract's limit applied): one line per change.
    uint8_t back00 = 0;
    uint8_t back02 = 0;
    int readErr = tegra_i2c_read(kBus, kCharger, bq::InputSource, &back00, 1);
    if (!readErr)
        readErr = tegra_i2c_read(kBus, kCharger, bq::ChargeCurrent, &back02, 1);
    char contract[40] = "";
    if (pd.inputLimit)
        snprintf(contract, sizeof(contract), " %u mV", pd.millivolts);
    IOLog("SwitchBattery: input limit %u mA (was %u mA%s): %s%s; read back REG00 %#04x (IINLIM %u mA, VINDPM %u mV%s) "
          "REG02 %#04x (ICHG %u mA)%s%s\n",
          bq::decodeInputLimit(want), found, hiZ ? ", Hi-Z" : "", why, contract, back00, bq::decodeInputLimit(back00),
          bq::decodeInputVoltage(back00), (back00 & bq::kHiZ) ? ", Hi-Z" : "", back02, bq::decodeChargeCurrent(back02),
          (!readErr && (back00 & (bq::kHiZ | bq::kIinlimMask)) != want) ? ": NOT what was written" : "",
          err || readErr ? " (I2C error)" : "");
    statsDue = true;
}

// One os_log line: input (source, voltage, limit), charge (current, settings, state), battery, raw status/fault.
void SwitchBattery::stats(const Reading &r, const PdSnapshot &pd)
{
    static const char *const kCharge[] = {"not charging", "pre-charge", "fast charge", "done"};
    const uint8_t s = r.charger[bq::SystemStatus];
    const unsigned vbus = bq::vbusStat(s);
    const bool good = (s & bq::kPowerGood) != 0;
    const char *source = inputSource(boosting, good, pd, vbus);
    char celsius[16];
    formatCentiC(celsius, sizeof(celsius), r.centiCelsius);
    os_log(
        OS_LOG_DEFAULT,
        "SwitchBattery: stats: in %s %u mV, limit %u mA%s | charge %d mA (avg %d), %s, ICHG %u mA VREG %u mV, zone %s "
        "%s | battery %u %% %u mV %s C | REG08 %#04x REG09 %#04x",
        source,
        pd.inputLimit ? pd.millivolts
        : good        ? 5000u
                      : 0u,
        bq::decodeInputLimit(r.charger[bq::InputSource]), (s & bq::kDpm) ? " (in DPM: the input limit binds)" : "",
        r.milliamps, r.avgMilliamps, kCharge[bq::chargeStat(s)], bq::decodeChargeCurrent(r.charger[bq::ChargeCurrent]),
        bq::decodeChargeVoltage(r.charger[bq::ChargeVoltage]), zoneControl ? charge::zoneName(zone) : "unknown",
        managed ? "" : " (charger as found)", r.percent, r.millivolts, celsius, s, r.fault);
}

// What iPadOS writes to the battery service ([IOS-PD], [IOS-PUI], [IOS-CLTM] in SwitchChargePolicy.h), on our work
// loop: the requests change, the decision follows (charge::decide), one line per change. Keys we do not know are
// refused (kIOReturnUnsupported), as IOService does.
IOReturn SwitchBattery::setProperties(OSObject *properties)
{
    OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
    if (!dict || !loop)
        return kIOReturnBadArgument;
    return loop->runAction(&SwitchBattery::requestAction, this, dict);
}

IOReturn SwitchBattery::requestAction(OSObject *owner, void *dict, void *, void *, void *)
{
    return static_cast<SwitchBattery *>(owner)->request(static_cast<const OSDictionary *>(dict));
}

IOReturn SwitchBattery::request(const OSDictionary *dict)
{
    charge::Requests next = requests;
    bool known = false;
    if (const OSBoolean *b = OSDynamicCast(OSBoolean, dict->getObject("IsCharging"))) {
        next.osInhibit = !b->getValue();
        known = true;
    }
    if (const OSBoolean *b = OSDynamicCast(OSBoolean, dict->getObject("ExternalConnected"))) {
        next.inflowDisabled = !b->getValue();
        known = true;
    }
    if (const OSBoolean *b = OSDynamicCast(OSBoolean, dict->getObject("PredictiveChargingInhibit"))) {
        next.predictiveInhibit = b->getValue();
        known = true;
    }
    if (const OSNumber *n = OSDynamicCast(OSNumber, dict->getObject("AppleChargeRateLimitIndex"))) {
        next.thermalLimitIndex = n->unsigned32BitValue();
        known = true;
    }
    if (!known)
        return kIOReturnUnsupported;
    if (next.osInhibit != requests.osInhibit) {
        IOLog("SwitchBattery: powerd's ChargeInhibit assertion %s\n",
              next.osInhibit ? "held: charging off" : "released");
    }
    if (next.predictiveInhibit != requests.predictiveInhibit) {
        IOLog("SwitchBattery: PowerUI PredictiveChargingInhibit %s\n",
              next.predictiveInhibit ? "on: charging held (Optimized Battery Charging)" : "off");
    }
    if (next.inflowDisabled != requests.inflowDisabled) {
        IOLog("SwitchBattery: powerd's DisableInflow assertion %s\n",
              next.inflowDisabled ? "held: input off" : "released: input on");
    }
    if (next.thermalLimitIndex != requests.thermalLimitIndex) {
        os_log(OS_LOG_DEFAULT, "SwitchBattery: thermalmonitord AppleChargeRateLimitIndex %u (charge current %u %%)",
               next.thermalLimitIndex,
               charge::kThermalRatePercent[next.thermalLimitIndex > 3 ? 3 : next.thermalLimitIndex]);
    }
    requests = next;
    const charge::Decision d = charge::decide(zone, requests);
    if (managed && !charge::sameDecision(d, decision)) {
        apply(d, "iPadOS request", lastCentiCelsius);
    } else if (!managed && zoneControl) {
        decision = d;
        const bool charges = decision.charge;
        (void)applyReg01(&charges, nullptr, nullptr, nullptr, nullptr);
    }
    statsDue = true;
    if (published)
        timer->setTimeoutMS(kSettleMS);  // republish (and Hi-Z/input limit, applyInput) after the change
    return kIOReturnSuccess;
}

// System sleep (SwitchSuspend, SC7): only the battery's zone edges may wake it (the gauge's ALRT, PMC wake 27, armed
// while it reads high): the SOC and voltage windows off, the temperature window the zone's own (no thermalmonitord
// degree), no re-read until the wake. Linux arms the gauge the same way (OLED DTB battery-gauge@36
// nvidia,pmc-wakeup event 27); switchroot's bq2419x keeps charging in suspend with its last settings.
void SwitchBattery::sleep(bool entering)
{
    if (!entering) {
        if (published)
            timer->setTimeoutMS(kSettleMS);  // what changed while asleep, and the normal windows
        return;
    }
    timer->cancelTimeout();
    const uint16_t window = zoneControl ? charge::temperatureWindow(zone, lastCentiCelsius) : 0x7f80;
    uint16_t status = 0;
    int err = gaugeWrite(SAlrtTh, 0xff00);
    if (!err)
        err = gaugeWrite(VAlrtTh, 0xff00);
    if (!err)
        err = gaugeWrite(TAlrtTh, window);
    if (!err)
        err = gaugeWord(Status, &status);
    if (!err && (status & ~kStatusKeep))
        err = gaugeWrite(Status, status & kStatusKeep);
    if (err)
        IOLog("SwitchBattery: gauge sleep window failed (I2C %d)\n", err);
}

// The charger's and gauge's alert lines through TegraGPIO (which may sleep until the GIC is up).
void SwitchBattery::attachAlerts()
{
    for (unsigned i = 0; i < kAlertCount; ++i) {
        IOService *nub = tegra_gpio_interrupt_nub(kAlerts[i].gpio, kAlerts[i].trigger);
        alerts[i] =
            nub ? IOInterruptEventSource::interruptEventSource(this, &SwitchBattery::alertFired, nub, 0) : nullptr;
        if (!alerts[i] || loop->addEventSource(alerts[i]) != kIOReturnSuccess) {
            panic("SwitchBattery: cannot attach %s (GPIO %u)", kAlerts[i].name, kAlerts[i].gpio);
        }
    }
    for (auto *alert : alerts)
        alert->enable();
}

// Work loop: one of the chips reported a change. Read everything a moment later (the charger, PD controller and
// gauge tend to fire together on a plug event). A line that keeps firing is stuck: panic and name it.
void SwitchBattery::alertFired(OSObject *owner, IOInterruptEventSource *sender, int)
{
    auto *self = OSDynamicCast(SwitchBattery, owner);
    if (!self)
        return;
    unsigned i = 0;
    while (i < kAlertCount && self->alerts[i] != sender)
        ++i;
    if (i == kAlertCount)
        return;
    const uint64_t now = mach_absolute_time();
    uint64_t elapsedNS = 0;
    absolutetime_to_nanoseconds(now - self->alertWindowStart[i], &elapsedNS);
    if (elapsedNS > 1000000000ULL) {
        self->alertWindowStart[i] = now;
        self->alertCount[i] = 0;
    }
    if (++self->alertCount[i] > kAlertBurst) {
        char state[256];
        tegra_gpio_describe(kAlerts[i].gpio, state, sizeof(state));
        panic("SwitchBattery: %s fired %u times in %llu ms, %s", kAlerts[i].name, self->alertCount[i],
              elapsedNS / 1000000, state);
    }
    self->timer->setTimeoutMS(kSettleMS);
}

// SwitchPD's work loop, after each BM92T36 event and each step of its USB-PD request. The state is kept for our loop;
// a change (a plug, a contract published or withdrawn) is re-read and applied after the settle time.
void SwitchBattery::pdChanged(void *context, const SwitchPDState *state)
{
    auto *self = static_cast<SwitchBattery *>(context);
    const uint64_t word = packPd(state);
    const uint64_t old = __atomic_exchange_n(&self->pdWord, word, __ATOMIC_ACQ_REL);
    // Before the first reading the identify timer runs on its own (and that reading takes this word).
    if (!__atomic_load_n(&self->published, __ATOMIC_ACQUIRE))
        return;
    if (old != word || state->alert)
        self->timer->setTimeoutMS(kSettleMS);
}

void SwitchBattery::timerFired(OSObject *owner, IOTimerEventSource *sender)
{
    auto *self = OSDynamicCast(SwitchBattery, owner);
    if (!self)
        return;
    if (self->published) {
        // An alert pending at the GPIO controller that never reached us panics here (TegraGPIO.h).
        for (const Alert &alert : kAlerts)
            tegra_gpio_check_delivery(alert.gpio, "SwitchBattery");
        switch_pd_check_delivery("SwitchBattery");
        ++self->refreshes;
        self->refresh();
    } else if (self->identify() && self->refresh()) {  // the first reading also arms the gauge window
        __atomic_store_n(&self->published, true, __ATOMIC_RELEASE);
        self->registerService();
        IOLog("SwitchBattery: started\n");
    }
    sender->setTimeoutMS(self->published ? kRefreshMS : kIdentifyRetryMS);
}

// The gauge's average current at the last reading (mA, positive = charging; INT32_MIN before a reading) and whether
// external power was in (then it is the charger's net, not the console's draw). For SwitchPerf's periodic line, which
// sets clocks against draw. Any context.
extern "C" int switch_battery_avg_ma(bool *external)
{
    if (external)
        *external = __atomic_load_n(&gBatteryExternal, __ATOMIC_RELAXED);
    return __atomic_load_n(&gBatteryAvgMa, __ATOMIC_RELAXED);
}

static SwitchBattery *gBatteryService;  // set once started (switch_battery_suspend/resume)

extern "C" int switch_battery_start(void)
{
    static SwitchBattery *battery;
    if (battery)
        return 0;
    IOService *resources = IOService::getResourceService();
    SwitchBattery *candidate = OSTypeAlloc(SwitchBattery);
    if (!resources || !candidate || !candidate->init() || !candidate->attach(resources)) {
        IOLog("SwitchBattery: attach failed\n");
        OSSafeReleaseNULL(candidate);
        return -1;
    }
    if (!candidate->start(resources)) {
        candidate->detach(resources);
        candidate->release();
        return -1;
    }
    battery = candidate;
    __atomic_store_n(&gBatteryService, candidate, __ATOMIC_RELEASE);
    return 0;
}

// NOLINTNEXTLINE(misc-const-correctness): IOWorkLoop::Action fixes the signature
static IOReturn sleepAction(OSObject *owner, void *entering, void *, void *, void *)
{
    static_cast<SwitchBattery *>(owner)->sleep(entering != nullptr);
    return kIOReturnSuccess;
}

// SwitchSuspend (SwitchBattery.h). Thread context, I2C up.
extern "C" void switch_battery_suspend(void)
{
    if (SwitchBattery *b = __atomic_load_n(&gBatteryService, __ATOMIC_ACQUIRE))
        b->getWorkLoop()->runAction(&sleepAction, b, b);
}

extern "C" void switch_battery_resume(void)
{
    if (SwitchBattery *b = __atomic_load_n(&gBatteryService, __ATOMIC_ACQUIRE))
        b->getWorkLoop()->runAction(&sleepAction, b, nullptr);
}

extern "C" int switch_battery_gauge_sample(struct switch_battery_gauge *sample)
{
    uint16_t qh = 0;
    uint16_t avg = 0;
    uint16_t now = 0;
    uint16_t vcell = 0;
    int err = gaugeWord(QH, &qh);
    if (!err)
        err = gaugeWord(AvgCurrent, &avg);
    if (!err)
        err = gaugeWord(Current, &now);
    if (!err)
        err = gaugeWord(VCell, &vcell);
    if (err)
        return err;
    sample->qh = qh;
    sample->avg_ma = (int16_t)avg * 15625 / 100000;
    sample->now_ma = (int16_t)now * 15625 / 100000;
    sample->mv = (vcell >> 3) * 625u / 1000;
    return 0;
}

// thermalmonitord's battery sensors (SwitchBattery.h). Any context.
extern "C" int switch_battery_temperature(int *milli_c, unsigned long long *at)
{
    for (unsigned tries = 0; tries < 4; ++tries) {
        const uint32_t g1 = __atomic_load_n(&gTempGen, __ATOMIC_ACQUIRE);
        const int value = __atomic_load_n(&gTempMilliC, __ATOMIC_RELAXED);
        const uint64_t when = __atomic_load_n(&gTempAt, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if ((g1 & 1) || g1 != __atomic_load_n(&gTempGen, __ATOMIC_RELAXED))
            continue;
        if (!when)
            return 0;
        *milli_c = value;
        *at = when;
        return 1;
    }
    return 0;
}

// VBUS for a sink-only partner (drivers/SwitchUSB/SwitchPD; SwitchBattery.h). Thread context.
extern "C" int switch_battery_set_otg_boost(bool on, bool ourVbus)
{
    IOLock *lock = boostLock();
    if (!lock)
        return -1;
    IOLockLock(lock);
    uint8_t status = 0;
    bool refused = false;
    int err = tegra_i2c_init(kBus);
    if (!err && on && !gBoosting && !ourVbus) {
        err = tegra_i2c_read(kBus, kCharger, bq::SystemStatus, &status, 1);
        refused =
            (err == 0) && (((status & bq::kPowerGood) != 0) || bq::vbusStat(status) == 1 || bq::vbusStat(status) == 2);
    }
    if (!err && !refused) {
        const bool was = gBoosting;
        gBoosting = on;
        err = reg01Locked(nullptr, nullptr);
        if (err)
            gBoosting = was;
    }
    IOLockUnlock(lock);
    if (err)
        IOLog("SwitchBattery: OTG boost %s failed (I2C %d)\n", on ? "on" : "off", err);
    if (err)
        return -1;
    return refused ? SWITCH_BATTERY_BOOST_VBUS_PRESENT : 0;
}

// The USB role report's charger line (SwitchBattery.h). Writes nothing; REG09's faults go through readFaults, so the
// next reading still sees what this read cleared.
extern "C" int switch_battery_describe_otg(char *buffer, size_t size)
{
    if (!buffer || !size)
        return -1;
    static const char *const kVbusStat[4] = {"none", "USB host", "adapter", "OTG"};
    static const char *const kChrgStat[4] = {"not charging", "pre-charge", "fast charge", "done"};
    static const char *const kWatchdog[4] = {"off", "40 s", "80 s", "160 s"};
    uint8_t config = 0;
    uint8_t timer = 0;
    uint8_t status = 0;
    uint8_t latched = 0;
    uint8_t now = 0;
    uint16_t current = 0;
    int err = tegra_i2c_init(kBus);
    if (!err)
        err = tegra_i2c_read(kBus, kCharger, bq::PowerOnConfig, &config, 1);
    if (!err)
        err = tegra_i2c_read(kBus, kCharger, bq::TerminationTimer, &timer, 1);
    if (!err)
        err = tegra_i2c_read(kBus, kCharger, bq::SystemStatus, &status, 1);
    if (!err)
        err = readFaults(&latched, &now);
    const int gauge = err ? err : gaugeWord(Current, &current);
    if (err) {
        snprintf(buffer, size, "BQ24193 not readable (I2C %d)", err);
        return err;
    }
    snprintf(buffer, size,
             "BQ24193 REG01 %#x (CHG_CONFIG %u, %s%s) REG05 %#x (watchdog %s) REG08 %#x (VBUS %s, %s, PG %u) REG09 "
             "%#x/%#x (boost fault %u/%u, watchdog fault %u/%u); battery %d mA",
             config, (config & bq::kChgConfigMask) >> 4, (config & bq::kBoostLim1300) ? "1.3 A" : "500 mA",
             __atomic_load_n(&gBoosting, __ATOMIC_RELAXED) ? ", ours" : "", timer,
             kWatchdog[(static_cast<unsigned>(timer) >> 4) & 3], status, kVbusStat[static_cast<unsigned>(status) >> 6],
             kChrgStat[(static_cast<unsigned>(status) >> 4) & 3], flag(status, bq::kPowerGood), latched, now,
             flag(latched, 0x40), flag(now, 0x40), flag(latched, 0x80), flag(now, 0x80),
             gauge ? 0 : (int16_t)current * 15625 / 100000);
    return 0;
}
