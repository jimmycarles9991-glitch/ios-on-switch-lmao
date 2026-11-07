// SPDX-License-Identifier: BSD-2-Clause-Patent
// What XNU's platform layer needs from the Switch OLED's (T210B01) PMICs on I2C5 (PWR_I2C), early in boot:
//   * the boot report: one console line with the CPU and EMC clocks as the CAR has them and VDD_CPU as the MAX77812
//     has it (SwitchClocks.h), always;
//   * the wall clock: the MAX77620 RTC, published as the "IORTC" resource that ArmDTPlatform forwards
//     PEGetUTCTimeOfDay() / PESetUTCTimeOfDay() to, read only (rtcMode 1; 2 would write it back);
//   * Shut Down and Restart: PE_halt_restart, which IOPlatformExpert::haltRestart() calls from PEHaltRestart();
//   * the EMC (DRAM) clock: 1600 MHz, asked of hekate's BPMP firmware over the L4T mailbox (drivers/TegraBPMP); the
//     loader has usually raised it already (boot/loader/emc.h). Afterwards the clock line is printed again;
//   * the thermal service (SwitchThermal.cpp, and with it the CPU operating point), started here rather than on
//     the board thread after the root disk: the fan is under control and the CPU raised before the root mount
//     (the board thread's later call is then a no-op).
// switch_pmic_start() is called by XNU's IOKitInitializeTime() on the bsd_init thread, right before
// clock_initialize_calendar() reads the platform clock, so the calendar starts at the RTC's time instead of 1970.
// It sleeps on I2C5 transfers (interrupt-driven, TegraI2C), on BPMP answers (interrupt-driven, TegraBPMP) and, the
// first time, until the GIC is up.
//
// MAX77620 RTC (Linux drivers/rtc/rtc-max77686.c max77620_drv_data; hekate bdk/rtc/max77620-rtc.{c,h}): its own I2C
// address 0x68. Writing RBUDR (UPDATE0 bit 4) copies the counters into SEC 0x07 .. DATE 0x0D (sec, min, hour,
// weekday one-hot, month 1-12, year 0-99 from 2000, day of month); writing them and then UDR (bit 0) loads them.
// Either takes 16 ms before the registers can be trusted (Linux .delay = 16000 us). Registers are written one at a
// time (tegra210b01-nx-pmic.dtsi maxim,avoid-rtc-bulk-register-write); reads may be bulk. CONTROL 0x03: bit 0 BCD,
// bit 1 24-hour; L4T forces binary 24-hour at probe (max77686_rtc_init_reg: CONTROLM 0x02 = mask), and so do we
// before the first write. The RTC holds whatever was last written: L4T and Android keep it in UTC; Horizon is said to
// use it as a steady clock plus an offset kept in its settings (not verified here), so on a console that only ran
// Horizon the first boot may start at the RTC's own date until timed sets the time, which is then written back in
// UTC, as L4T does. Horizon then shows that shift until it next syncs its own clock.
//
// Shut Down: userspace reboot(RB_HALT) -> reboot_kernel() (sync, unmount, buffers clean) -> host_reboot() ->
// halt_all_cpus(FALSE) -> PEHaltRestart(kPEHaltCPU) -> IOPMrootDomain's shutdown notifications -> IOPlatformExpert::
// haltRestart() -> PE_halt_restart; Restart the same with kPERestartCPU. Both end here, in that thread:
//   1. the panel goes dark first, as hekate's power_set_state() does (hw_deinit -> display_end) and L4T's suspend list
//      for this panel: TegraPanel's brightness 0, DCS display off, sleep in, while the DC still scans out;
//   2. the MAX77620 soft reset, through tegra_i2c_final_transfer(): polled, I2C5 kept from then on (TegraI2C.h).
// Power-off (Linux drivers/power/reset/max77620-poweroff.c max77620_pm_power_off, selected by the DT's
// maxim,system-pmic-power-off; hekate power_set_state(POWER_OFF_RESET)): read ONOFFIRQ 0x0B and IRQTOP 0x05 to clear
// what is latched, clear ONOFFCNFG2 0x42 SFT_RST_WK (bit 7) and, as hekate does, WK_ALARM1/WK_ALARM2 (bits 2, 1) so an
// RTC alarm left by Horizon (it uses ALARM2) does not wake the console, then set ONOFFCNFG1 0x41 SFT_RST (bit 7). The
// PMIC then runs its power-down sequence and stays off until the power button (hekate comes up next). PSCI SYSTEM_OFF
// is no option: switchroot ATF's tegra_soc_prepare_system_off() panics "operation not handled".
// Restart: the same with SFT_RST_WK set, a full PMIC power cycle: switchroot U-Boot's reset_misc() does exactly this
// on T210B01 ("r2p is not possible on T210B01, so do a full power off reboot", board/nintendo/switch), and hekate's
// POWER_OFF_REBOOT the same. If that fails, PSCI SYSTEM_RESET, L4T Linux's path (pmc.c restart handler -> psci):
// switchroot ATF tegra_soc_prepare_system_reset() sets SCLK to its POR default and writes PMC_CNTRL MAIN_RST; should
// that return, PMC_CNTRL MAIN_RST from here (hekate's reboot, L4T tegra_pmc_restart).
// A power-off or restart that does not happen panics with one line, the panel lit again first so that it is seen: a
// console left dark and running would pass for off and drain the battery.
//
// Panics keep their screen: PE_halt_restart is also handed the panic callouts and, from halt_all_cpus(TRUE) at the
// end of a panic, kPERestartCPU; with a panic in progress it returns -1 and XNU spins with the panic on the display,
// as it did with PE_halt_restart unset (SwitchPanicSave.c decides about a reset there).
//
// The Wi-Fi/Bluetooth module's 32.768 kHz sleep clock (the BCM4356's LPO) comes from the MAX77620, set as L4T sets it
// when the PMIC is probed (switchroot tegra210b01-nx-pmic.dtsi): GPIO4 in its alternate function 32K_OUT1 (pin_gpio4
// function "32k-out1"; Linux pinctrl-max77620.c sets AME_GPIO 0x40 bit 4) and the 32 kHz output enabled
// (maxim,enable-clock32k-out: CNFG1_32K 0x03 OUT0_EN, bit 2). Neither hekate nor switchroot U-Boot sets either, and
// the soft reset above puts the PMIC back to its OTP defaults, GPIO4 a plain GPIO. Before r7a XNU's halt never
// reached the soft reset, so the PMIC kept what Horizon or L4T had set. Without the clock (r7h, every boot after an
// r7a+ power-off) the Bluetooth ROM answers HCI Reset and Read Local Version, acknowledges the 3 Mbaud switch (0xFC18)
// and is never heard from again; Wi-Fi gets by on the module's internal oscillator. switch_pmic_start() sets both,
// before the board thread brings up Wi-Fi and Bluetooth.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/rtc/IORTCController.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/clock.h>
#include <os/log.h>
#include <pexpert/pexpert.h>
#include "../../diagnostics/switch/SwitchPanicSave.h"
#include "../../diagnostics/switch/SwitchPowerTrail.h"
#include "../SwitchBrightness/TegraPanel.h"
#include "../TegraBPMP/TegraBPMP.h"
#include "../TegraI2C/TegraI2C.h"
#include "SwitchClocks.h"
#include "SwitchEMC.h"
#include "SwitchMAX77620.h"
#include "SwitchMonotonicClock.h"
#include "SwitchPower.h"

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" boolean_t preemption_enabled(void);
extern "C" unsigned panic_active(void);

namespace {
using namespace max77620;

enum : uint8_t {
    IRQTOP = 0x05,
    ONOFFIRQ = 0x0b,
    ONOFFCNFG1 = 0x41,
    ONOFFCNFG2 = 0x42
};

constexpr uint8_t kSoftReset = 1u << 7;                                                      // ONOFFCNFG1 SFT_RST
constexpr uint8_t kWakeOnSoftReset = 1u << 7, kWakeAlarm1 = 1u << 2, kWakeAlarm2 = 1u << 1;  // ONOFFCNFG2

// The module's 32 kHz clock (top of file; hekate bdk/power/max77620.h, Linux pinctrl-max77620.c).
enum : uint8_t {
    CNFG1_32K = 0x03,
    AME_GPIO = 0x40
};

constexpr uint8_t k32kOut0Enable = 1u << 2, kGpio4Alternate = 1u << 4;  // CNFG1_32K OUT0_EN; AME_GPIO GPIO4 32K_OUT1

// Sleep wake alarm (Linux rtc-max77686.c, hekate max77620-rtc.h): RTCINT 0x00 (clear on read) / RTCINTM 0x01, bit 1 =
// ALARM1; ALARM1 SEC 0x0E .. DATE 0x14 laid out like the time registers, bit 7 of each = compare this field. The
// alarm reaches the SoC as the PMIC's nIRQ through IRQTOP bit 3 (RTC), masked in IRQTOPM 0x0D.
enum : uint8_t {
    IRQTOPM = 0x0d
};

constexpr uint8_t kRtcAlarm1 = 1u << 1, kRtcIntMaskAll = 0x3f, kAlarmCompare = 1u << 7, kTopRtc = 1u << 3;
constexpr uint8_t kRtcBcd = 1u << 0, kRtc24Hour = 1u << 1, kRtcHourPm = 1u << 6;
constexpr uint32_t kPowerDownUS = 2000000;  // the PMIC cuts power within milliseconds of SFT_RST
// PMC_CNTRL (APBDEV_PMC_CNTRL, PMC at 0x7000E400) MAIN_RST: the SoC's warm reset (hekate, L4T tegra_pmc_restart).
constexpr vm_offset_t kPmcPA = 0x7000E000;
constexpr uint32_t kPmcCntrl = 0x400, kPmcMainReset = 1u << 4;
volatile uint32_t *gPmc;  // mapped at start: nothing is mapped on the restart path

// The Wi-Fi/Bluetooth module's 32 kHz clock on, as L4T has it (top of file); read-modify-write, other bits kept.
// One line to the log (not the console) when it was off, which is every boot after a PMIC power-off.
int moduleClockOn()
{
    uint8_t cnfg = 0, ame = 0;
    int err = readByte(kPmic, CNFG1_32K, &cnfg);
    if (!err)
        err = readByte(kPmic, AME_GPIO, &ame);
    if (err)
        return err;
    const uint8_t cnfgOn = cnfg | k32kOut0Enable, ameOn = ame | kGpio4Alternate;
    if (cnfgOn != cnfg)
        err = writeByte(kPmic, CNFG1_32K, cnfgOn);
    if (!err && ameOn != ame)
        err = writeByte(kPmic, AME_GPIO, ameOn);
    if (!err && (cnfgOn != cnfg || ameOn != ame)) {
        os_log(OS_LOG_DEFAULT,
               "SwitchPMIC: Wi-Fi/Bluetooth 32 kHz clock on (CNFG1_32K %#04x -> %#04x, AME_GPIO %#04x -> %#04x)", cnfg,
               cnfgOn, ame, ameOn);
    }
    return err;
}

// Days since 1970-01-01 of a proleptic Gregorian date, and back (H. Hinnant's days_from_civil / civil_from_days).
int64_t daysFromCivil(int year, int month, int day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const int yoe = year - era * 400;
    const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
}

void civilFromDays(int64_t days, int *year, int *month, int *day)
{
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int doe = (int)(days - era * 146097);
    const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int mp = (5 * doy + 2) / 153;
    *day = doy - (153 * mp + 2) / 5 + 1;
    *month = mp < 10 ? mp + 3 : mp - 9;
    *year = (int)(yoe + era * 400) + (*month <= 2);
}

// Sets `bit` in UPDATE0 (the others, e.g. the auto-clear mode, stay) and waits out the transfer.
int rtcUpdate(uint8_t bit)
{
    rtcQuiet(true);
    uint8_t update = 0;
    int err = readByte(kRtc, RTC_UPDATE0, &update);
    if (!err)
        err = writeByte(kRtc, RTC_UPDATE0, update | bit);
    if (!err) {
        rtcUpdated();
        IOSleep(kRtcUpdateMS);
    }
    return err;
}

// The RTC's time in seconds since 1970 (UTC), or -1 with `why` filled in.
int64_t rtcRead(char *why, size_t size)
{
    uint8_t control = 0, t[kTimeRegs] = {};
    int err = rtcUpdate(kRtcReadUpdate);
    if (!err)
        err = readByte(kRtc, RTC_CONTROL, &control);
    if (!err)
        err = tegra_i2c_read(kBus, kRtc, RTC_SEC, t, sizeof(t));
    if (err) {
        snprintf(why, size, "I2C5 error %d", err);
        return -1;
    }
    const bool bcd = control & kRtcBcd;
    auto value = [bcd](uint8_t raw, uint8_t mask) {
        raw &= mask;
        return bcd ? (raw >> 4) * 10 + (raw & 15) : (int)raw;
    };
    int hour;
    if (control & kRtc24Hour) {
        hour = value(t[HOUR], bcd ? 0x3f : 0x1f);
    } else {
        hour = value(t[HOUR], 0x1f) % 12 + ((t[HOUR] & kRtcHourPm) ? 12 : 0);
    }
    const int sec = value(t[SEC], 0x7f), min = value(t[MIN], 0x7f), day = value(t[DATE], 0x3f);
    const int month = value(t[MONTH], 0x1f), year = 2000 + value(t[YEAR], bcd ? 0xff : 0x7f);
    if (sec > 59 || min > 59 || hour > 23 || month < 1 || month > 12 || day < 1 || day > 31 || year > 2099) {
        snprintf(why, size, "invalid date %04d-%02d-%02d %02d:%02d:%02d (CONTROL %#x)", year, month, day, hour, min,
                 sec, control);
        return -1;
    }
    return daysFromCivil(year, month, day) * 86400 + hour * 3600 + min * 60 + sec;
}

int rtcWrite(int64_t secs)
{
    if (secs < 946684800 || secs >= 4102444800)
        return TEGRA_I2C_EINVAL;  // the RTC holds 2000 .. 2099 only
    int year, month, day;
    const int64_t days = secs / 86400;
    const int rest = (int)(secs % 86400);
    civilFromDays(days, &year, &month, &day);
    const uint8_t t[kTimeRegs] = {
        (uint8_t)(rest % 60), (uint8_t)(rest / 60 % 60), (uint8_t)(rest / 3600), (uint8_t)(1u << ((days + 4) % 7)),
        (uint8_t)month,       (uint8_t)(year - 2000),    (uint8_t)day,
    };
    rtcQuiet(true);
    uint8_t control = 0;
    int err = readByte(kRtc, RTC_CONTROL, &control);
    if (!err && (control & (kRtcBcd | kRtc24Hour)) != kRtc24Hour) {
        err = writeByte(kRtc, RTC_CONTROLM, kRtcBcd | kRtc24Hour);
        if (!err)
            err = writeByte(kRtc, RTC_CONTROL, kRtc24Hour);
        if (!err)
            err = rtcUpdate(kRtcWriteUpdate);
    }
    for (unsigned i = 0; !err && i < kTimeRegs; ++i)
        err = writeByte(kRtc, (uint8_t)(RTC_SEC + i), t[i]);
    if (!err)
        err = rtcUpdate(kRtcWriteUpdate);
    return err;
}

// ALARM1 at `secs` (RTC time), each field in the RTC's current format (left as the boot chain set it: read-only mode
// must not change the RTC), all but the weekday compared; or, for secs < 0, every compare off.
int alarmWrite(int64_t secs)
{
    rtcQuiet(true);
    uint8_t control = 0;
    int err = readByte(kRtc, RTC_CONTROL, &control);
    uint8_t t[kTimeRegs] = {};
    if (!err && secs >= 0) {
        const bool bcd = control & kRtcBcd;
        auto field = [bcd](int value) {
            return (uint8_t)(bcd ? ((value / 10) << 4 | value % 10) : value);
        };
        int year, month, day;
        const int rest = (int)(secs % 86400);
        civilFromDays(secs / 86400, &year, &month, &day);
        const int hour = rest / 3600;
        t[SEC] = field(rest % 60) | kAlarmCompare;
        t[MIN] = field(rest / 60 % 60) | kAlarmCompare;
        t[HOUR] =
            (control & kRtc24Hour ? field(hour)
                                  : (uint8_t)(field(hour % 12 ? hour % 12 : 12) | (hour >= 12 ? kRtcHourPm : 0))) |
            kAlarmCompare;
        t[MONTH] = field(month) | kAlarmCompare;
        t[YEAR] = field(year - 2000) | kAlarmCompare;
        t[DATE] = field(day) | kAlarmCompare;
    }
    for (unsigned i = 0; !err && i < kTimeRegs; ++i)
        err = writeByte(kRtc, (uint8_t)(RTC_ALARM1 + i), t[i]);
    if (!err)
        err = rtcUpdate(kRtcWriteUpdate);
    return err;
}

// The terminal transfers, polled (TegraI2C.h tegra_i2c_final_transfer): the first one keeps I2C5 for good.
int finalRead(uint8_t reg, uint8_t *value)
{
    return tegra_i2c_final_transfer(kBus, kPmic, &reg, 1, value, 1);
}

int finalWrite(uint8_t reg, uint8_t value)
{
    const uint8_t data[2] = {reg, value};
    return tegra_i2c_final_transfer(kBus, kPmic, data, sizeof(data), nullptr, 0);
}

// MAX77620 soft reset: L4T max77620_pm_power_off / max77620_pm_power_reset (read-modify-write), hekate
// power_set_state(POWER_OFF_RESET / POWER_OFF_REBOOT). Restart keeps SFT_RST_WK set: the PMIC powers up again.
int pmicSoftReset(bool restart)
{
    uint8_t value = 0;
    if (!restart) {  // max77620_pm_power_off: clear the latched power-key and top-level interrupts
        (void)finalRead(ONOFFIRQ, &value);
        (void)finalRead(IRQTOP, &value);
    }
    int err = finalRead(ONOFFCNFG2, &value);
    if (!err) {
        value = restart ? (uint8_t)(value | kWakeOnSoftReset)
                        : (uint8_t)(value & ~(kWakeOnSoftReset | kWakeAlarm1 | kWakeAlarm2));
        err = finalWrite(ONOFFCNFG2, value);
    }
    if (!err)
        err = finalRead(ONOFFCNFG1, &value);
    if (!err)
        err = finalWrite(ONOFFCNFG1, value | kSoftReset);
    return err;
}

void psciSystemReset()
{
    // PSCI 0.2 SYSTEM_RESET over the /psci node's SMC conduit (ArmCpu's CPU_ON uses the same one).
    register uint64_t x0 __asm__("x0") = 0x84000009;
    __asm__ volatile("smc #0"
                     : "+r"(x0)
                     :
                     : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
                       "x16", "x17", "memory");
}

// PE_halt_restart. A Shut Down or Restart outside a panic ends here; everything else returns -1 (see the top).
int haltRestart(unsigned int type)
{
    if ((type != kPEHaltCPU && type != kPERestartCPU) || panic_active())
        return -1;
    const bool restart = type == kPERestartCPU;
    switch_power_trail("PE_halt_restart(%u)", type);
    // Storage needs nothing more: reboot_kernel() synced and unmounted every file system and waited out the dirty
    // buffers before halt_all_cpus(), and NativeSwitchBlock completes a write only once the card has it.
    bool dark = false;
    // XNU's path is thread context; the steps below sleep (the panel's commands ride DC frame ends). Each durable mark
    // is in the RTC before its step, the last one before the soft reset takes I2C5 for good.
    if (preemption_enabled()) {
        switch_power_trail_mark(SWITCH_MARK_HALT_PLATFORM);
        switch_power_trail_mark_flush();
        switch_thermal_cpu_quiesce();  // the DFLL, if the CPU runs on it, must not share I2C5 with what follows
        switch_power_trail_mark(SWITCH_MARK_HALT_CPU);
        switch_power_trail_mark_flush();
        dark = tegra_panel_power_off() == 0;
        switch_power_trail("panel %s", dark ? "dark" : "not running");
        switch_power_trail_mark(SWITCH_MARK_HALT_PMIC);
        switch_power_trail_mark_flush();
    }
    const int err = pmicSoftReset(restart);
    switch_power_trail("MAX77620 soft reset written: %d", err);
    if (!err)
        IODelay(kPowerDownUS);  // busy: interrupts may be off here
    switch_power_trail_mark_now(SWITCH_MARK_HALT_NOT_OFF);
    if (restart) {
        psciSystemReset();
        if (gPmc)
            gPmc[kPmcCntrl / 4] |= kPmcMainReset;
        IODelay(kPowerDownUS);
    }
    if (dark)
        (void)tegra_panel_power_on(-1);
    if (restart) {
        panic("SwitchPMIC: restart failed: MAX77620 soft reset %s (%d), PSCI SYSTEM_RESET and PMC main reset returned",
              err ? "write failed" : "did not take", err);
    }
    panic("SwitchPMIC: power-off failed: MAX77620 soft reset %s (%d)", err ? "write failed" : "did not take", err);
}
}  // namespace

// The wall clock between boots. Reads come from the boot-time RTC reading plus elapsed mach time (no I2C on the
// read path, which XNU also takes on wake paths); settimeofday() writes the RTC back in rtcMode 2.
class SwitchRTC : public IORTC {
    OSDeclareDefaultStructors(SwitchRTC) IOSimpleLock *clockLock = nullptr;
    IOLock *writeLock = nullptr;
    uint64_t baseNS = 0, baseStamp = 0;  // wall time in ns since 1970 at mach_absolute_time() == baseStamp
    uint64_t rtcNS = 0, rtcStamp = 0;    // the last RTC reading (ns since 1970) and its mach_absolute_time()
    bool writeBack = false;
    void setBase(uint64_t ns, uint64_t stamp);

public:
    bool initAt(int64_t secs, uint64_t stamp, bool write);
    void free() override;
    long getGMTTimeOfDay() override;
    void setGMTTimeOfDay(long secs) override;
    void getUTCTimeOfDay(clock_sec_t *secs, clock_nsec_t *nsecs) override;
    void setUTCTimeOfDay(clock_sec_t secs, clock_nsec_t nsecs) override;
    void setAlarmEnable(IOOptionBits message) override;
    IOReturn getMonotonicClockAndTimestamp(uint64_t *usecs, uint64_t *machTime) override;
    IOReturn getMonotonicClockOffset(int64_t *usecs) override;
    // System sleep (SwitchSuspend): powerd's next scheduled wake as ALARM1 before SC7; after it, the wall clock is read
    // from the RTC again (mach time stood still while the SoC was off) and the alarm is turned off.
    void registerWakeSettings();
    void suspend();
    int resume();
    static IOReturn wakeSetting(OSObject *target, const OSSymbol *type, OSObject *value, uintptr_t refcon);
    int64_t wakeAt = 0;  // wall time (s) of the next scheduled wake, 0 = none
    bool alarmArmed = false;
    volatile bool alarmFired = false;  // RTCINT ALARM1 seen (here or by SwitchButtons' PMIC interrupt)
};

OSDefineMetaClassAndStructors(SwitchRTC, IORTC)

    bool SwitchRTC::initAt(int64_t secs, uint64_t stamp, bool write)
{
    if (!IORTC::init())
        return false;
    clockLock = IOSimpleLockAlloc();
    writeLock = IOLockAlloc();
    if (!clockLock || !writeLock)
        return false;
    baseNS = (uint64_t)secs * NSEC_PER_SEC;
    rtcNS = baseNS;
    rtcStamp = stamp;
    baseStamp = stamp;
    writeBack = write;
    return true;
}

void SwitchRTC::free()
{
    if (clockLock)
        IOSimpleLockFree(clockLock);
    if (writeLock)
        IOLockFree(writeLock);
    IORTC::free();
}

void SwitchRTC::setBase(uint64_t ns, uint64_t stamp)
{
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(clockLock);
    baseNS = ns;
    baseStamp = stamp;
    IOSimpleLockUnlockEnableInterrupt(clockLock, state);
}

void SwitchRTC::getUTCTimeOfDay(clock_sec_t *secs, clock_nsec_t *nsecs)
{
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(clockLock);
    const uint64_t ns = baseNS, stamp = baseStamp;
    IOSimpleLockUnlockEnableInterrupt(clockLock, state);
    uint64_t elapsed = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - stamp, &elapsed);
    *secs = (clock_sec_t)((ns + elapsed) / NSEC_PER_SEC);
    *nsecs = (clock_nsec_t)((ns + elapsed) % NSEC_PER_SEC);
}

// XNU calls this from clock_set_calendar_microtime() with interrupts enabled ("this call might block").
void SwitchRTC::setUTCTimeOfDay(clock_sec_t secs, clock_nsec_t nsecs)
{
    setBase((uint64_t)secs * NSEC_PER_SEC + nsecs, mach_absolute_time());
    if (!writeBack)
        return;
    IOLockLock(writeLock);
    const int err = rtcWrite((int64_t)secs);
    if (!err) {
        rtcNS = (uint64_t)secs * NSEC_PER_SEC;  // the RTC now reads the wall clock (sleep resync offset 0)
        rtcStamp = mach_absolute_time();
    }
    IOLockUnlock(writeLock);
    if (err)
        IOLog("SwitchRTC: RTC write failed (%d), it keeps its old time\n", err);
}

long SwitchRTC::getGMTTimeOfDay()
{
    clock_sec_t secs = 0;
    clock_nsec_t nsecs = 0;
    getUTCTimeOfDay(&secs, &nsecs);
    return (long)secs;
}

void SwitchRTC::setGMTTimeOfDay(long secs)
{
    setUTCTimeOfDay((clock_sec_t)secs, 0);
}

IOReturn SwitchRTC::getMonotonicClockAndTimestamp(uint64_t *usecs, uint64_t *machTime)
{
    return switch_monotonic_clock_read(usecs, machTime) ? kIOReturnSuccess : kIOReturnUnsupported;
}

IOReturn SwitchRTC::getMonotonicClockOffset(int64_t *usecs)
{
    return switch_monotonic_clock_offset(usecs) ? kIOReturnSuccess : kIOReturnUnsupported;
}

// No IORTC alarm messages: scheduled wakes come through the root domain's PM settings (registerWakeSettings), and
// power-off clears the alarm wake sources.
void SwitchRTC::setAlarmEnable(IOOptionBits message)
{}

// IOPMrootDomain forwards powerd's scheduled wake (IOPMSchedulePowerEvent) as "wake" (seconds from now, 0 = none) or
// "WakeByCalendarDate" (IOPMCalendarStruct, UTC) to the controllers registered for it (IOPMrootDomain.cpp setPMSetting).
void SwitchRTC::registerWakeSettings()
{
    IOPMrootDomain *root = IOService::getPMRootDomain();
    const OSSymbol *settings[] = {OSSymbol::withCString(kIOPMSettingAutoWakeSecondsKey),
                                  OSSymbol::withCString(kIOPMSettingAutoWakeCalendarKey), nullptr};
    OSObject *handle = nullptr;
    if (!root || !settings[0] || !settings[1] ||
        root->registerPMSettingController(settings, &SwitchRTC::wakeSetting, this, 0, &handle) != kIOReturnSuccess) {
        IOLog("SwitchRTC: cannot register for scheduled wakes; the console only wakes on the power key\n");
    }
    OSSafeReleaseNULL(settings[0]);
    OSSafeReleaseNULL(settings[1]);
}

IOReturn SwitchRTC::wakeSetting(OSObject *target, const OSSymbol *type, OSObject *value, uintptr_t)
{
    auto *self = OSDynamicCast(SwitchRTC, target);
    if (!self || !type)
        return kIOReturnBadArgument;
    clock_sec_t now = 0;
    clock_nsec_t nsecs = 0;
    self->getUTCTimeOfDay(&now, &nsecs);
    if (type->isEqualTo(kIOPMSettingAutoWakeSecondsKey)) {
        OSNumber *seconds = OSDynamicCast(OSNumber, value);
        self->wakeAt = seconds && seconds->unsigned32BitValue() ? (int64_t)now + seconds->unsigned32BitValue() : 0;
    } else if (type->isEqualTo(kIOPMSettingAutoWakeCalendarKey)) {
        OSData *data = OSDynamicCast(OSData, value);
        const IOPMCalendarStruct *date = data && data->getLength() >= sizeof(IOPMCalendarStruct)
                                             ? (const IOPMCalendarStruct *)data->getBytesNoCopy()
                                             : nullptr;
        self->wakeAt = date && date->year >= 2000 ? daysFromCivil(date->year, date->month, date->day) * 86400 +
                                                        date->hour * 3600 + date->minute * 60 + date->second
                                                  : 0;
    }
    return kIOReturnSuccess;
}

// Sleep action, thread context, I2C still up. The RTC may run at an offset from the wall clock (read-only mode keeps
// Horizon's RTC as it is), so the alarm is set in RTC time: RTC now + (wake - wall now).
void SwitchRTC::suspend()
{
    alarmFired = false;
    alarmArmed = false;
    clock_sec_t now = 0;
    clock_nsec_t nsecs = 0;
    getUTCTimeOfDay(&now, &nsecs);
    if (wakeAt <= (int64_t)now + 1)
        return;
    char why[96] = "";
    uint8_t stale = 0, top = 0;
    const int64_t rtcNow = rtcRead(why, sizeof(why));
    int err = rtcNow < 0 ? TEGRA_I2C_EIO : alarmWrite(rtcNow + (wakeAt - (int64_t)now));
    if (!err)
        err = readByte(kRtc, RTC_INT, &stale);
    if (!err)
        err = writeByte(kRtc, RTC_INT_MASK, (uint8_t)(kRtcIntMaskAll & ~kRtcAlarm1));
    if (!err)
        err = readByte(kPmic, IRQTOPM, &top);
    if (!err)
        err = writeByte(kPmic, IRQTOPM, (uint8_t)(top & ~kTopRtc));
    if (err) {
        IOLog("SwitchRTC: wake alarm not set (%s %d); the console sleeps until the power key\n", why, err);
        return;
    }
    alarmArmed = true;
}

// Wake action, right after the I2C buses are back. Returns 1 when ALARM1 is what fired.
int SwitchRTC::resume()
{
    char why[96] = "";
    const int64_t secs = rtcRead(why, sizeof(why));
    if (secs >= 0) {
        // Mach time stood still in SC7 for both the wall clock and the RTC's extrapolation, so their difference (the
        // offset settimeofday() gave the wall clock) is unchanged and carries over to the new reading.
        const uint64_t now = mach_absolute_time();
        uint64_t elapsed = 0;
        absolutetime_to_nanoseconds(now - rtcStamp, &elapsed);
        clock_sec_t wall = 0;
        clock_nsec_t nsecs = 0;
        getUTCTimeOfDay(&wall, &nsecs);
        const int64_t offsetNS = ((int64_t)wall * (int64_t)NSEC_PER_SEC + nsecs) - (int64_t)(rtcNS + elapsed);
        rtcNS = (uint64_t)secs * NSEC_PER_SEC;
        rtcStamp = now;
        setBase((uint64_t)((int64_t)rtcNS + offsetNS), now);
    } else {
        IOLog("SwitchRTC: RTC unreadable after sleep (%s); the clock misses the time asleep\n", why);
    }
    if (!alarmArmed)
        return 0;
    alarmArmed = false;
    uint8_t pending = 0, top = 0;
    int err = readByte(kRtc, RTC_INT, &pending);
    if (!err && (pending & kRtcAlarm1))
        alarmFired = true;
    if (!err)
        err = writeByte(kRtc, RTC_INT_MASK, kRtcIntMaskAll);
    if (!err)
        err = readByte(kPmic, IRQTOPM, &top);
    if (!err)
        err = writeByte(kPmic, IRQTOPM, (uint8_t)(top | kTopRtc));
    if (!err)
        err = alarmWrite(-1);
    if (err)
        IOLog("SwitchRTC: wake alarm not cleared (I2C5 %d)\n", err);
    if (alarmFired)
        wakeAt = 0;  // powerd schedules the next one
    return alarmFired ? 1 : 0;
}

namespace {
void reportClocks(const char *when)
{
    using namespace switchclk;
    volatile uint8_t *car = (volatile uint8_t *)ml_io_map(kCarPA, 0x1000);
    if (!car) {
        IOLog("SwitchClocks %s: CAR not mapped\n", when);
        return;
    }
    const Cpu cpu = readCpu(car);
    const Emc emc = readEmc(car);
    char cpuRate[16], pllRate[16], emcRate[16], rail[48];
    formatMHz(cpuRate, sizeof(cpuRate), cpu.kHz);
    formatMHz(pllRate, sizeof(pllRate), pllKHz(cpu.pllx, refKHz(car), 8, false));
    formatMHz(emcRate, sizeof(emcRate), emc.kHz);
    uint8_t address = 0, vout = 0;
    const int err = readCpuRail(&address, &vout);
    if (err)
        snprintf(rail, sizeof(rail), "VDD_CPU unreadable (I2C5 %d)", err);
    else
        snprintf(rail, sizeof(rail), "VDD_CPU %u mV (MAX77812 0x%02x)", voutMilliVolts(vout), address);
    IOLog("SwitchClocks %s: CPU %s from %s (CCLK %#x; PLLX %s = %u.%u x %u / %u / %u), %s; EMC %s from %s"
          " (CLK_SOURCE_EMC %#x)\n",
          when, cpuRate, cpuSourceName(cpu.source), cpu.policy, pllRate, refKHz(car) / 1000, refKHz(car) % 1000 / 100,
          (cpu.pllx >> 8) & 0xff, cpu.pllx & 0xff, qlinDivider(cpu.pllx), rail, emcRate, emcSourceName(emc.source),
          emc.clockSource);
}

SwitchRTC *publishRTC(unsigned mode)
{
    char why[96] = "";
    const int64_t secs = rtcRead(why, sizeof(why));
    const uint64_t stamp = mach_absolute_time();
    if (secs < 0) {
        IOLog("SwitchRTC: MAX77620 RTC not used (%s); the clock starts at 1970\n", why);
        return nullptr;
    }
    IOService *resources = IOService::getResourceService();
    SwitchRTC *rtc = OSTypeAlloc(SwitchRTC);
    if (!resources || !rtc || !rtc->initAt(secs, stamp, mode >= 2) || !rtc->attach(resources)) {
        IOLog("SwitchRTC: could not create the RTC service; the clock starts at 1970\n");
        OSSafeReleaseNULL(rtc);
        return nullptr;
    }
    rtc->setName("SwitchRTC");
    rtc->registerService();
    IOService::publishResource("IORTC", rtc);
    int year, month, day;
    civilFromDays(secs / 86400, &year, &month, &day);
    const int rest = (int)(secs % 86400);
    IOLog("SwitchRTC: %04d-%02d-%02d %02d:%02d:%02d UTC from the MAX77620 RTC%s\n", year, month, day, rest / 3600,
          rest / 60 % 60, rest % 60, mode >= 2 ? " (settimeofday writes it back)" : "");
    return rtc;
}

SwitchRTC *gRTC;
}  // namespace

// System sleep hooks (SwitchSuspend), and the PMIC interrupt's RTC part (SwitchButtons owns the nIRQ): reading RTCINT
// releases the line; an ALARM1 seen there is remembered for the wake reason.
extern "C" void switch_rtc_sleep_enable(void)
{
    if (gRTC)
        gRTC->registerWakeSettings();
}

extern "C" void switch_rtc_suspend(void)
{
    if (gRTC)
        gRTC->suspend();
}

extern "C" IOReturn switch_rtc_utc_usec(uint64_t *usec)
{
    if (!gRTC)
        return kIOReturnNotReady;
    clock_sec_t secs = 0;
    clock_nsec_t nsecs = 0;
    gRTC->getUTCTimeOfDay(&secs, &nsecs);
    *usec = (uint64_t)secs * USEC_PER_SEC + nsecs / NSEC_PER_USEC;
    return kIOReturnSuccess;
}

extern "C" int switch_rtc_resume(void)
{
    return gRTC ? gRTC->resume() : 0;
}

#if DEVELOPMENT || DEBUG
// SwitchSuspend's journal before SC7: the PMIC's sleep and wake setup as the SoC leaves it, against what L4T's
// max77620_i2c_suspend sets (ONOFFCNFG1 SLPEN bit 2 for the DT's FPS sleep; ONOFFCNFG2 WK_EN0 bit 0 cleared).
extern "C" void switch_pmic_sleep_report(char *line, size_t size)
{
    static const uint8_t regs[] = {ONOFFCNFG1, ONOFFCNFG2, IRQTOPM, 0x0e, 0x12, 0x43, 0x44, 0x45, 0x4f};
    uint8_t v[sizeof(regs)] = {}, rtcMask = 0;
    int err = 0;
    for (size_t i = 0; i < sizeof(regs) && !err; ++i)
        err = readByte(kPmic, regs[i], &v[i]);
    if (!err)
        err = readByte(kRtc, RTC_INT_MASK, &rtcMask);
    snprintf(line, size,
             "ONOFFCNFG1 %#04x ONOFFCNFG2 %#04x IRQTOPM %#04x INTENLBT %#04x ONOFFIRQM %#04x FPS_CFG %#04x %#04x %#04x "
             "FPS_SD0 %#04x RTCINTM %#04x (I2C5 %d)",
             v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], rtcMask, err);
}
#endif

extern "C" void switch_rtc_interrupt(void)
{
    uint8_t pending = 0;
    if (!readByte(kRtc, RTC_INT, &pending) && (pending & kRtcAlarm1) && gRTC)
        gRTC->alarmFired = true;
}

extern "C" int switch_pmic_start(void)
{
    static bool started;
    if (started)
        return 0;
    started = true;
    reportClocks("at boot");
    (void)switch_monotonic_clock_start();

    // Read-only: Horizon keeps its clock in the same RTC, and writing ours would shift it.
    constexpr unsigned rtcMode = 1;
    constexpr unsigned emcKHz = 1600000;  // hekate's top stock MTC table, L4T's T210B01 core-DVFS "emc" ceiling
    uint8_t control = 0;
    const int err = tegra_i2c_init(kBus) ? TEGRA_I2C_ENOTREADY : readByte(kPmic, ONOFFCNFG1, &control);
    if (err) {
        IOLog("SwitchPMIC: MAX77620 not answering on I2C5 (%d): no RTC, no power-off\n", err);
    } else {
        if (const int clockErr = moduleClockOn()) {
            IOLog("SwitchPMIC: Wi-Fi/Bluetooth 32 kHz clock not set (%d): Bluetooth will not come up\n", clockErr);
        }
        gRTC = publishRTC(rtcMode);
        int recordMillic = 0;
        const enum switch_thermal_sensor recorded = switch_pmic_thermal_take(&recordMillic);
        switch_panic_save_thermal(recorded, recordMillic);
#if DEVELOPMENT || DEBUG
        switch_pmic_mark_start();
#endif
        gPmc = (volatile uint32_t *)ml_io_map(kPmcPA, 0x1000);
        PE_halt_restart = haltRestart;
    }

    {
        char line[256];
        const int emcErr = tegra_bpmp_emc_boot(emcKHz, line, sizeof(line));
        IOLog("SwitchEMC: %s\n", line);
        if (!emcErr) {
            reportClocks("after EMC");
            switch_emc_start();
        }
    }
    if (switch_thermal_start())
        IOLog("SwitchPMIC: SwitchThermal did not start, the board thread retries\n");
    return err ? -1 : 0;
}
