// SPDX-License-Identifier: BSD-2-Clause-Patent
// The durable power mark (diagnostics/switch/SwitchPowerTrail.h): one code byte in the MAX77620 RTC's ALARM2, the one
// store on the board that a forced power-off keeps, since the RTC runs from the battery while the PMIC is off.
// The RTC drops a field written out of its range at the update and keeps the old value (on 11-06 every halt code, 96 and
// up, left ALARM2_SEC at the boot's 0x1f while ALARM2_MIN counted on), so every field stays in range: the code's low five
// bits in ALARM2_SEC, its high two in ALARM2_HOUR, how many marks the boot wrote (mod 60) in ALARM2_MIN, kMagic in
// ALARM2_YEAR, 1 + the flags (SWITCH_MARK_FLAG_*) in ALARM2_DATE, and weekday and month 1. Each field's bit 7, its
// compare enable, stays clear, so the alarm never matches (Linux max77686_rtc_stop_alarm turns an alarm off the same way,
// and the power-off clears ONOFFCNFG2 WK_ALARM2 besides); ALARM1 is the wake alarm (SwitchPMIC.cpp). Horizon programs
// ALARM2 for its own alarms, so a Horizon boot in between leaves no mark of ours; hekate's max77620_rtc_stop_alarm clears
// only the compare bits and keeps the mark.
// Debug kernels write marks; every kernel keeps the RTC's update rule (SwitchMAX77620.h) and the software thermal
// shutdown's record (SwitchThermalRecord.h), which the marks give way to once written.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include "../../diagnostics/switch/SwitchPowerTrail.h"
#include "SwitchMAX77620.h"
#include "SwitchThermalRecord.h"

namespace {
uint64_t gQuietAt;  // mach_absolute_time() from which the RTC takes register writes and updates again (atomic)
bool gThermalHeld;  // a thermal shutdown's record is in ALARM2: no mark replaces it (atomic)
}  // namespace

void max77620::rtcQuiet(bool canSleep)
{
    uint64_t interval = 0;
    nanoseconds_to_absolutetime(uint64_t{kRtcUpdateMS} * 1000000, &interval);
    for (;;) {
        const uint64_t now = mach_absolute_time();
        const uint64_t quiet = __atomic_load_n(&gQuietAt, __ATOMIC_ACQUIRE);
        // Further off than one update: the counter restarted (the first C after SC7, before cpu_timebase_init
        // rebases mach time), and the last update is long done.
        if (now >= quiet || quiet - now > interval)
            return;
        if (!canSleep)
            continue;
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(quiet - now, &ns);
        IOSleep(static_cast<unsigned>(ns / 1000000) + 1);
    }
}

void max77620::rtcUpdated()
{
    uint64_t interval = 0;
    nanoseconds_to_absolutetime(uint64_t{kRtcUpdateMS} * 1000000, &interval);
    __atomic_store_n(&gQuietAt, mach_absolute_time() + interval, __ATOMIC_RELEASE);
}

#if DEVELOPMENT || DEBUG
namespace {
using namespace max77620;

constexpr uint8_t kMagic = 91;  // 90 was the out-of-range layout
constexpr uint8_t kAlarmCompare = 1u << 7;
constexpr uint32_t kNoMark = 0x100;  // gWritten before this boot's first mark

uint32_t gPending = kNoMark;  // the latest mark asked for (atomic)
uint32_t gWritten = kNoMark;  // the latest mark in the RTC (gLock, or the one caller of switch_pmic_mark_now)
uint8_t gCount;               // marks written this boot
uint8_t gFlags;               // SWITCH_MARK_FLAG_* set this boot, ALARM2_DATE - 1
uint8_t gUpdate0;             // RTCUPDATE0's other bits as the boot found them
IOLock *gLock;
thread_call_t gCall;

using Write = int (*)(uint8_t reg, uint8_t value);

constexpr uint8_t alarm2(unsigned field)
{
    return static_cast<uint8_t>(RTC_ALARM2 + field);
}

int threadWrite(uint8_t reg, uint8_t value)
{
    return writeByte(kRtc, reg, value);
}

int rawWrite(uint8_t reg, uint8_t value)
{
    const uint8_t data[2] = {reg, value};
    return tegra_i2c_raw_write(kBus, kRtc, data, sizeof(data));
}

// A field's value in range for the binary 24-hour layout (Linux max77686_rtc_tm_to_data).
uint8_t fieldValue(unsigned field, uint32_t code, uint8_t count)
{
    switch (field) {
    case SEC:
        return static_cast<uint8_t>(code & 0x1fu);
    case MIN:
        return count;
    case HOUR:
        return static_cast<uint8_t>((code >> 5) & 0x3u);
    case YEAR:
        return kMagic;
    case DATE:
        return static_cast<uint8_t>(1u + gFlags);
    default:
        return 1;  // weekday (one-hot), month
    }
}

// The code and the count, then UDR. A boot's first mark writes every field, compare bits clear: whatever alarm ALARM2
// held is off from then on.
int writeMark(Write put, uint32_t code)
{
    if (__atomic_load_n(&gThermalHeld, __ATOMIC_ACQUIRE)) {
        gWritten = code;  // taken as written, so commitLocked's loop ends
        return 0;
    }
    const auto count = static_cast<uint8_t>((gCount + 1u) % 60u);
    const unsigned fields = gWritten == kNoMark ? kTimeRegs : HOUR + 1u;
    int err = 0;
    for (unsigned field = SEC; !err && field < fields; ++field)
        err = put(alarm2(field), fieldValue(field, code, count));
    if (!err)
        err = put(RTC_UPDATE0, gUpdate0 | kRtcWriteUpdate);
    rtcUpdated();
    if (!err) {
        gCount = count;
        gWritten = code;
    }
    return err;
}

// Writes marks until the latest is in; under gLock.
void commitLocked()
{
    for (;;) {
        const uint32_t code = __atomic_load_n(&gPending, __ATOMIC_ACQUIRE);
        if (code == gWritten || code == kNoMark)
            return;
        rtcQuiet(true);
        if (const int err = writeMark(threadWrite, code)) {
            IOLog("SwitchPowerTrail: durable mark %#04x not written (I2C5 %d)\n", code, err);
            return;
        }
    }
}

void writerCall(thread_call_param_t, thread_call_param_t)
{
    IOLockLock(gLock);
    commitLocked();
    IOLockUnlock(gLock);
}

// The last boot's mark, read through RBUDR; -1 if ALARM2 holds none of ours.
int lastMark(unsigned *count, unsigned *flags)
{
    uint8_t alarm[kTimeRegs] = {};
    rtcQuiet(true);
    int err = writeByte(kRtc, RTC_UPDATE0, gUpdate0 | kRtcReadUpdate);
    if (!err) {
        rtcUpdated();
        rtcQuiet(true);
        err = tegra_i2c_read(kBus, kRtc, RTC_ALARM2, alarm, sizeof(alarm));
    }
    for (const uint8_t field : alarm) {
        if (field & kAlarmCompare)
            return -1;
    }
    if (err || alarm[YEAR] != kMagic || alarm[SEC] > 0x1fu || alarm[HOUR] > 0x3u || alarm[DATE] < 1 || alarm[DATE] > 4)
        return -1;
    *count = alarm[MIN];
    *flags = alarm[DATE] - 1u;
    return static_cast<int>((unsigned{alarm[HOUR]} << 5) | alarm[SEC]);
}
}  // namespace

// SwitchPMIC.cpp's start, once the MAX77620 answers (thread context): the last boot's mark is logged and this boot's
// first replaces it.
extern "C" void switch_pmic_mark_start(void)
{
    uint8_t update = 0;
    if (const int err = readByte(kRtc, RTC_UPDATE0, &update)) {
        IOLog("SwitchPowerTrail: no durable marks this boot (RTCUPDATE0: I2C5 %d)\n", err);
        return;
    }
    gUpdate0 = static_cast<uint8_t>(update & ~(unsigned{kRtcWriteUpdate} | kRtcReadUpdate));
    unsigned count = 0;
    unsigned flags = 0;
    const int last = lastMark(&count, &flags);
    switch_power_trail_durable(last, count, flags);
    gLock = IOLockAlloc();
    gCall =
        gLock ? thread_call_allocate_with_options(writerCall, nullptr, THREAD_CALL_PRIORITY_KERNEL_HIGH, 0) : nullptr;
    if (!gCall) {
        IOLog("SwitchPowerTrail: no durable marks this boot (no lock or thread call)\n");
        return;
    }
    switch_power_trail_mark(SWITCH_MARK_BOOT_PMIC);
    switch_pmic_mark_flush();
}

extern "C" void switch_pmic_mark(uint8_t code)
{
    __atomic_store_n(&gPending, code, __ATOMIC_RELEASE);
    if (gCall)
        thread_call_enter(gCall);
}

extern "C" void switch_pmic_mark_flush(void)
{
    if (!gLock)
        return;
    IOLockLock(gLock);
    commitLocked();
    rtcQuiet(true);
    IOLockUnlock(gLock);
}

extern "C" void switch_pmic_mark_now(uint8_t code)
{
    __atomic_store_n(&gPending, code, __ATOMIC_RELEASE);
    if (!gCall)
        return;
    rtcQuiet(false);
    (void)writeMark(rawWrite, code);
}

// ALARM2_DATE alone (1 + the flags), then UDR: the code and the count stay as the last mark left them.
extern "C" void switch_pmic_flag_now(uint8_t flag)
{
    if (!gCall || __atomic_load_n(&gThermalHeld, __ATOMIC_ACQUIRE))
        return;
    gFlags = static_cast<uint8_t>((unsigned{gFlags} | flag) & 0x3u);
    rtcQuiet(false);
    if (!rawWrite(alarm2(DATE), static_cast<uint8_t>(1u + gFlags)))
        (void)rawWrite(RTC_UPDATE0, gUpdate0 | kRtcWriteUpdate);
    rtcUpdated();
}
#endif

namespace {
int updateBits(uint8_t *bits)
{
    uint8_t update = 0;
    const int err = max77620::readByte(max77620::kRtc, max77620::RTC_UPDATE0, &update);
    *bits = static_cast<uint8_t>(update & ~(unsigned{max77620::kRtcWriteUpdate} | max77620::kRtcReadUpdate));
    return err;
}

int writeAlarm2(const uint8_t *fields, unsigned count, uint8_t update)
{
    int err = 0;
    for (unsigned field = 0; !err && field < count; ++field)
        err = max77620::writeByte(max77620::kRtc, static_cast<uint8_t>(max77620::RTC_ALARM2 + field), fields[field]);
    if (!err)
        err = max77620::writeByte(max77620::kRtc, max77620::RTC_UPDATE0, update | max77620::kRtcWriteUpdate);
    max77620::rtcUpdated();
    max77620::rtcQuiet(true);
    return err;
}
}  // namespace

extern "C" int switch_pmic_thermal_record(enum switch_thermal_sensor sensor, int millic)
{
    uint8_t fields[SWITCH_THERMAL_RECORD_FIELDS];
    uint8_t update = 0;
    switch_thermal_record_encode(sensor, millic, fields);
    __atomic_store_n(&gThermalHeld, true, __ATOMIC_RELEASE);
#if DEVELOPMENT || DEBUG
    // A mark being written now finishes first; none is written after.
    if (gLock)
        IOLockLock(gLock);
#endif
    max77620::rtcQuiet(true);
    int err = updateBits(&update);
    if (!err)
        err = writeAlarm2(fields, SWITCH_THERMAL_RECORD_FIELDS, update);
#if DEVELOPMENT || DEBUG
    if (gLock)
        IOLockUnlock(gLock);
#endif
    return err;
}

extern "C" enum switch_thermal_sensor switch_pmic_thermal_take(int *millic)
{
    uint8_t fields[SWITCH_THERMAL_RECORD_FIELDS] = {};
    uint8_t update = 0;
    max77620::rtcQuiet(true);
    int err = updateBits(&update);
    if (!err) {
        err = max77620::writeByte(max77620::kRtc, max77620::RTC_UPDATE0, update | max77620::kRtcReadUpdate);
        max77620::rtcUpdated();
        max77620::rtcQuiet(true);
    }
    if (!err)
        err = tegra_i2c_read(max77620::kBus, max77620::kRtc, max77620::RTC_ALARM2, fields, sizeof(fields));
    const enum switch_thermal_sensor sensor = err ? SWITCH_THERMAL_NONE : switch_thermal_record_decode(fields, millic);
    if (sensor != SWITCH_THERMAL_NONE) {
        const uint8_t noMagic[max77620::YEAR + 1] = {fields[0], fields[1], fields[2], fields[3], fields[4], 0};
        (void)writeAlarm2(noMagic, max77620::YEAR + 1, update);
    }
    return sensor;
}
