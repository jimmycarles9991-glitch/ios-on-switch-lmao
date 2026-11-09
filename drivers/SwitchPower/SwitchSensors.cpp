// SPDX-License-Identifier: BSD-2-Clause-Patent
// thermalmonitord's sensors on the Switch (SwitchSensors.h has the mapping, CONTRACT.md the contract and evidence).
//
// iPadOS's CLTM daemon reads every temperature sensor of its product model (the IPSW's iPad: J120AP, J181AP) every 5 s
// through the HID event system and reports itself unhealthy to watchdogd until all of them answer; without them it also
// has nothing to mitigate on. Here each of those sensors is a HID service (SwitchHIDDevice's temperature kind) whose copyEvent answers from the
// Switch sensor nearest to what the iPad's sensor measures:
//   * SOC_THERM (T210B01, 0x700E2000): six on-die tsensors, cpu0-cpu3 beside each A57 core, gpu, pllx, brought up as
//     L4T's soctherm.c does for "nvidia,tegra210b01-soctherm" (tegra210-soctherm.c tegra210b01_tsensors, fuse
//     calibration from tsensor-fuse.c; clocks 51 MHz from PLLP and 400 kHz from CLK_M, tegra210-soc-base.dtsi). The
//     hardware converts continuously; a read is the TS_*_STATUS1 register at the moment thermalmonitord asks (its own
//     5 s cadence, through its read path: no timer here). Once up, SOC_THERM's group thresholds and their interrupt
//     belong to SwitchSocTherm.cpp; no throttle or THERMTRIP is set up.
//   * TMP451 (SwitchThermal): SwitchThermal's last good reading of the SoC diode and the PCB, which it takes on every
//     ALERT (the sensor's window around the fan levels) and every degree SOC_THERM's die groups move; once SwitchThermal
//     loses the sensor it is a read failure. One reader of the chip, the fan's.
//   * MAX17050 (SwitchBattery): the battery thermistor, kept current by the gauge's temperature alert window (a degree
//     either way, SwitchBattery.cpp) besides its other alerts.
//   * PMU tcal: the iPad PMU's calibration channel, answered with thermalmonitord's own reference (51.82 C,
//     SwitchSensorModels.h): it adds that reference minus this to the PMU thermistors, and the Switch's sensors are
//     digital and need no correction.
// A source that cannot answer makes that read fail (no event), and thermalmonitord reports the sensor missing; nothing
// is ever made up. Every value is the measurement and its time.
//
// Each device's IOHIDEventDriver gets the copyEvent hook (SwitchHID.h) from IOKit's first-publish notification or,
// when that has not fired, from hookPass once the device's matching is quiet. The publish notification alone has
// never been seen to hook a sensor on hardware; hookPass's line says which path did.
//
// The userspace watchdog (NativeSwitchWatchdog) asks switch_sensors_wait_ready() once, when watchdogd first opens
// it: only once thermalmonitord itself has read every sensor it reads, and every source answers, does watchdogd keep
// monitoring thermalmonitord; otherwise it is off for the boot (one line with what is missing) and an unhealthy
// thermalmonitord can never end in a watchdog panic. "Read" means the hook returned an event to a client's copyEvent
// for that sensor: hooked alone does not prove that thermalmonitord's reads reach the hook.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <os/log.h>
#include "../SwitchTouch/SwitchHID.h"
#include "../SwitchBattery/SwitchBattery.h"
#include "SwitchPower.h"
#include "SwitchSensors.h"
#include "SwitchSocTherm.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"

#ifndef SWITCH_IPSW_BUILD
#error "SwitchSensors.h takes the thermal model of the IPSW kind's iPad: build with driver_flags.bzl's IPSW_KIND_FLAGS"
#endif

namespace {
constexpr vm_offset_t kCarPA = 0x60006000, kFusePA = 0x7000F800, kSocThermPA = 0x700E2000;

// CAR (hekate bdk/soc/clock.h; L4T clk-tegra-periph.c): SOC_THERM is peripheral 78 (bank U bit 14, reset too), TSENSOR
// 100 (bank V bit 4, clock only). CLK_SOURCE_SOC_THERM is MUX8 (31:29: 0 CLK_M, 1 PLLC, 2 PLLP, 3 PLLA), TSENSOR MUX
// (31:30: 0 PLLP, 1 PLLC, 2 CLK_M, 3 CLK_32K); both 7.1 dividers in 7:0.
enum : uint32_t {
    RST_DEVICES_U = 0x00c,
    CLK_OUT_ENB_U = 0x018,
    MISC_CLK_ENB = 0x048,
    RST_DEV_U_SET = 0x310,
    RST_DEV_U_CLR = 0x314,
    CLK_ENB_H_SET = 0x328,
    CLK_ENB_U_SET = 0x330,
    CLK_OUT_ENB_V = 0x360,
    CLK_SOURCE_TSENSOR = 0x3b8,
    CLK_ENB_V_SET = 0x440,
    CLK_SOURCE_SOC_THERM = 0x644,
};

constexpr uint32_t kSocThermBit = 1u << 14, kTsensorBit = 1u << 4;
constexpr uint32_t kSocThermSource = (2u << 29) | ((408000 / 51000 - 1) * 2u);  // PLLP / 8 = 51 MHz
constexpr uint32_t kTsensorSource = (2u << 30) | ((19200 / 400 - 1) * 2u);      // CLK_M / 48 = 400 kHz
constexpr uint32_t kFuseClock = 1u << 7, kFuseVisible = 1u << 28;  // as SwitchThermal, TegraDFLL, TegraGM20B
constexpr uint32_t FUSE_CACHE = 0x100;

// SOC_THERM (Linux soctherm.h / soctherm.c).
enum : uint32_t {
    CONFIG0 = 0x0,
    CONFIG1 = 0x4,
    CONFIG2 = 0x8,
    STATUS1 = 0x10,
    SENSOR_PDIV = 0x1c0,
    HOTSPOT_OFF = 0x1c4
};

constexpr uint32_t kTempValid = 1u << 31;
constexpr uint32_t kPdivCpuShift = 12, kPdivGpuShift = 8, kPdivPllxShift = 0;  // 4 bits each
constexpr uint32_t kHotspotCpuShift = 16, kHotspotGpuShift = 8;                // 8 bits each
constexpr uint32_t kHotspotCpu = 10, kHotspotGpu = 5;                          // pllx_hotspot_diff

constexpr uint32_t kHookQuietMS = 10000;  // for all the devices together
static_assert(kHookQuietMS < 41000, "IOService::waitQuiet reports (and may panic on) busy services from 41 s on");

volatile uint8_t *gSocTherm;                     // mapped and brought up: the tsensors can be read
volatile uint8_t *gCar, *gFuse, *gSocThermRegs;  // mapped once, reused by the bring-up after SC7
IOService *gDevices[SWITCH_SENSOR_COUNT];
bool gStarted, gVerdictLogged;
IOLock *gLock;
uint32_t gEvent;       // what switch_sensors_wait_ready sleeps on
uint32_t gSourceDown;  // atomic: bit per source that failed its last read after the sensors were ready
uint64_t gServed;      // atomic: bit per sensor whose hook has returned an event to a client
const uint64_t kReadMask = switch_sensors_read_mask();
thread_call_t gHookCall;
const char *const kSourceNames[SWITCH_SRC_COUNT] = {
    "SOC_THERM cpu0", "SOC_THERM cpu1",   "SOC_THERM cpu2", "SOC_THERM cpu3",   "SOC_THERM gpu",
    "SOC_THERM pllx", "TMP451 SoC diode", "TMP451 PCB",     "MAX17050 battery", "tcal"};

// The lock switch_sensors_wait_ready sleeps under: created by whichever comes first, the start or the watchdog's wait
// (watchdogd can open the IOWatchdog before the board thread gets here; not started is then just not ready yet).
IOLock *sensorsLock()
{
    IOLock *lock = __atomic_load_n(&gLock, __ATOMIC_ACQUIRE);
    if (lock)
        return lock;
    IOLock *fresh = IOLockAlloc();
    IOLock *none = nullptr;
    if (!fresh)
        return nullptr;
    if (__atomic_compare_exchange_n(&gLock, &none, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return fresh;
    IOLockFree(fresh);
    return none;
}

// SOC_THERM bring-up. SC7 resets the block and its clocks, so this runs again at wake. False (one line) if a part is
// missing: those six sources then never answer.
bool socThermStart()
{
    if (!gCar)
        gCar = switch_mmio_map(kCarPA, 0x1000);
    if (!gFuse)
        gFuse = switch_mmio_map(kFusePA, 0x400);
    if (!gSocThermRegs)
        gSocThermRegs = switch_mmio_map(kSocThermPA, 0x1000);
    volatile uint8_t *car = gCar, *fuse = gFuse, *regs = gSocThermRegs;
    if (!car || !fuse || !regs) {
        IOLog("SwitchSensors: SOC_THERM off: mapping failed\n");
        return false;
    }
    // Fuses readable (the fuse clock and the fuse cache's visibility), then the calibration words.
    switch_mmio_write32(car, CLK_ENB_H_SET, kFuseClock);
    switch_mmio_write32(car, MISC_CLK_ENB, switch_mmio_read32(car, MISC_CLK_ENB) | kFuseVisible);
    (void)switch_mmio_read32(car, MISC_CLK_ENB);
    const uint32_t common = switch_mmio_read32(fuse, FUSE_CACHE + SWITCH_FUSE_TSENSOR_COMMON);
    const switch_tsensor_shared shared = switch_tsensor_shared_calib(common);
    uint32_t calib[SWITCH_SOCTHERM_SENSORS];
    for (unsigned i = 0; i < SWITCH_SOCTHERM_SENSORS; ++i) {
        const uint32_t word = switch_mmio_read32(fuse, FUSE_CACHE + switch_tsensors[i].calib_fuse);
        calib[i] = switch_tsensor_calib(&switch_tsensor_b01, &shared, &switch_tsensors[i], word);
        if (!calib[i] || !common) {
            IOLog("SwitchSensors: SOC_THERM off: %s calibration fuse %#x (common %#x) gives no calibration\n",
                  switch_tsensors[i].name, word, common);
            return false;
        }
    }
    // Clocks under reset, as L4T's soctherm_clk_enable (rates from its clock init table), then out of reset.
    switch_mmio_write32(car, RST_DEV_U_SET, kSocThermBit);
    switch_mmio_write32(car, CLK_SOURCE_SOC_THERM, kSocThermSource);
    switch_mmio_write32(car, CLK_SOURCE_TSENSOR, kTsensorSource);
    switch_mmio_write32(car, CLK_ENB_U_SET, kSocThermBit);
    switch_mmio_write32(car, CLK_ENB_V_SET, kTsensorBit);
    (void)switch_mmio_read32(car, CLK_OUT_ENB_V);
    IODelay(2);
    switch_mmio_write32(car, RST_DEV_U_CLR, kSocThermBit);
    (void)switch_mmio_read32(car, RST_DEVICES_U);
    if (!(switch_mmio_read32(car, CLK_OUT_ENB_U) & kSocThermBit) ||
        !(switch_mmio_read32(car, CLK_OUT_ENB_V) & kTsensorBit) ||
        (switch_mmio_read32(car, RST_DEVICES_U) & kSocThermBit)) {
        IOLog("SwitchSensors: SOC_THERM off: clocks did not come up (CLK_OUT_ENB_U %#x, V %#x, RST_DEVICES_U %#x)\n",
              switch_mmio_read32(car, CLK_OUT_ENB_U), switch_mmio_read32(car, CLK_OUT_ENB_V),
              switch_mmio_read32(car, RST_DEVICES_U));
        return false;
    }
    // soctherm_init: each tsensor's configuration and calibration, the groups' PDIV and hotspot offsets.
    for (unsigned i = 0; i < SWITCH_SOCTHERM_SENSORS; ++i) {
        const uint32_t base = switch_tsensors[i].base;
        switch_mmio_write32(regs, base + CONFIG0, switch_tsensor_config0(&switch_tsensor_b01));
        switch_mmio_write32(regs, base + CONFIG1, switch_tsensor_config1(&switch_tsensor_b01));
        switch_mmio_write32(regs, base + CONFIG2, calib[i]);
    }
    uint32_t pdiv = switch_mmio_read32(regs, SENSOR_PDIV);
    pdiv &= ~((0xfu << kPdivCpuShift) | (0xfu << kPdivGpuShift) | (0xfu << kPdivPllxShift));
    pdiv |= switch_tsensor_b01.pdiv << kPdivCpuShift | switch_tsensor_b01.pdiv << kPdivGpuShift |
            switch_tsensor_b01.pdiv << kPdivPllxShift;
    switch_mmio_write32(regs, SENSOR_PDIV, pdiv);
    uint32_t hotspot = switch_mmio_read32(regs, HOTSPOT_OFF);
    hotspot &= ~((0xffu << kHotspotCpuShift) | (0xffu << kHotspotGpuShift));
    hotspot |= kHotspotCpu << kHotspotCpuShift | kHotspotGpu << kHotspotGpuShift;
    switch_mmio_write32(regs, HOTSPOT_OFF, hotspot);
    (void)switch_mmio_read32(regs, HOTSPOT_OFF);
    __atomic_store_n(&gSocTherm, regs, __ATOMIC_RELEASE);
    switch_soctherm_armed(regs);
    IOLog(
        "SwitchSensors: SOC_THERM up (fuse common %#x; CONFIG2 cpu0 %#x cpu1 %#x cpu2 %#x cpu3 %#x gpu %#x pllx %#x)\n",
        common, calib[0], calib[1], calib[2], calib[3], calib[4], calib[5]);
    return true;
}

// One reading of a source: m C and its time. False when it cannot answer now.
bool read(switch_sensor_source source, int32_t *milliC, uint64_t *at)
{
    int soc = 0;
    int pcb = 0;
    int value = 0;
    unsigned long long when = 0;
    switch (source) {
    case SWITCH_SRC_SOC_CPU0:
    case SWITCH_SRC_SOC_CPU1:
    case SWITCH_SRC_SOC_CPU2:
    case SWITCH_SRC_SOC_CPU3:
    case SWITCH_SRC_SOC_GPU:
    case SWITCH_SRC_SOC_PLLX: {
        volatile uint8_t *regs = __atomic_load_n(&gSocTherm, __ATOMIC_ACQUIRE);  // nullptr during the wake bring-up
        if (!regs)
            return false;
        const uint32_t status = switch_mmio_read32(regs, switch_tsensors[source].base + STATUS1);
        if (!(status & kTempValid))
            return false;
        *milliC = switch_soctherm_millic(status & 0xffffu);
        *at = mach_absolute_time();
        break;
    }
    case SWITCH_SRC_TMP451_SOC:
    case SWITCH_SRC_TMP451_PCB: {
        if (!switch_thermal_tmp451(&soc, &pcb, &when))
            return false;
        *milliC = source == SWITCH_SRC_TMP451_SOC ? soc : pcb;
        *at = when;
        break;
    }
    case SWITCH_SRC_BATTERY:
        if (!switch_battery_temperature(&value, &when))
            return false;
        *milliC = value;
        *at = when;
        break;
    case SWITCH_SRC_TCAL:
        *milliC = SWITCH_SENSOR_TCAL_MILLIC;
        *at = mach_absolute_time();
        break;
    default:
        return false;
    }
    return switch_sensor_plausible(*milliC) != 0;
}

// After the sensors were found ready: one line when a source stops answering, one when it is back.
void noteSource(switch_sensor_source source, bool ok)
{
    if (!__atomic_load_n(&gVerdictLogged, __ATOMIC_ACQUIRE))
        return;
    const uint32_t bit = 1u << source;
    const uint32_t before = ok ? __atomic_fetch_and(&gSourceDown, ~bit, __ATOMIC_RELAXED)
                               : __atomic_fetch_or(&gSourceDown, bit, __ATOMIC_RELAXED);
    if (ok && (before & bit))
        IOLog("SwitchSensors: %s answers again\n", kSourceNames[source]);
    if (!ok && !(before & bit)) {
        IOLog("SwitchSensors: %s stopped answering: thermalmonitord reports its sensors missing\n",
              kSourceNames[source]);
    }
}

// SwitchHIDDevice's copyEvent handler, on thermalmonitord's call (any thread; maybe inside the HID services' shared
// command gate): a register read or two atomic loads, then the event. Only a sensor's first event wakes the verdict.
// NOLINTNEXTLINE(misc-const-correctness): SwitchHIDCopyEventHandler's signature
OSObject *answer(void *context)
{
    const uintptr_t index = reinterpret_cast<uintptr_t>(context);
    const auto &sensor = switch_sensors[index];
    int32_t milliC = 0;
    uint64_t at = 0;
    const bool ok = read(sensor.source, &milliC, &at);
    noteSource(sensor.source, ok);
    OSObject *event = ok ? switch_hid_temperature_event(at, switch_sensor_fixed(milliC)) : nullptr;
    if (event) {
        const uint64_t bit = 1ULL << index;
        if (!(__atomic_fetch_or(&gServed, bit, __ATOMIC_ACQ_REL) & bit))
            switch_sensors_note();
    }
    return event;
}

// Thread call, once. Sleeps in waitQuiet, which IOKit's busy count wakes.
void hookPass(thread_call_param_t, thread_call_param_t)
{
    uint64_t deadline = 0;
    clock_interval_to_deadline(kHookQuietMS, kMillisecondScale, &deadline);
    unsigned kinds[kSwitchHIDHookKinds] = {};
    unsigned unquiet = 0;
    unsigned missing = 0;
    for (IOService *const &slot : gDevices) {
        IOService *device = __atomic_load_n(&slot, __ATOMIC_ACQUIRE);
        if (!device) {
            ++missing;
            continue;
        }
        const uint64_t now = mach_absolute_time();
        uint64_t leftNS = 0;
        if (deadline > now)
            absolutetime_to_nanoseconds(deadline - now, &leftNS);
        if (device->waitQuiet(leftNS) != kIOReturnSuccess)
            ++unquiet;
        ++kinds[switch_hid_temperature_hook(device)];
    }
    const unsigned hooked = kinds[kSwitchHIDHooked] + kinds[kSwitchHIDHookedAtPublish];
    char line[320];
    const size_t n = static_cast<size_t>(snprintf(
        line, sizeof(line), "%u of %u HID sensors hooked (%u by the publish notification, %u once matching was quiet)",
        hooked, SWITCH_SENSOR_COUNT, kinds[kSwitchHIDHookedAtPublish], kinds[kSwitchHIDHooked]));
    if (hooked != SWITCH_SENSOR_COUNT && n < sizeof(line)) {
        snprintf(line + n, sizeof(line) - n,
                 "; not hooked: %u not published, %u with no event driver (%u not quiet after %u s), %u wrong slots, "
                 "%u other vtable, %u no memory",
                 missing, kinds[kSwitchHIDHookNoDriver], unquiet, kHookQuietMS / 1000, kinds[kSwitchHIDHookWrongSlots],
                 kinds[kSwitchHIDHookOtherVtable], kinds[kSwitchHIDHookNoMemory]);
    }
    IOLog("SwitchSensors: %s\n", line);
    switch_sensors_note();
}

// Whether thermalmonitord's health check can pass; `why` lists what is missing.
bool ready(char *why, size_t len)
{
    size_t n = 0;
    if (why && len)
        why[0] = 0;
    const auto add = [&](const char *what) {
        if (why && n < len)
            n += static_cast<size_t>(snprintf(why + n, len - n, "%s%s", n ? ", " : "", what));
    };
    unsigned hooked = 0;
    for (IOService *const &slot : gDevices) {
        IOService *device = __atomic_load_n(&slot, __ATOMIC_ACQUIRE);
        // An event driver that matched after hookPass's wait.
        if (device &&
            (switch_hid_temperature_hooked(device) || switch_hid_temperature_hook(device) == kSwitchHIDHooked))
            ++hooked;
    }
    if (hooked != SWITCH_SENSOR_COUNT) {
        char what[64];
        snprintf(what, sizeof(what), "%u of %u HID sensors hooked", hooked, SWITCH_SENSOR_COUNT);
        add(what);
    }
    const uint64_t served = __atomic_load_n(&gServed, __ATOMIC_ACQUIRE) & kReadMask;
    if (served != kReadMask) {
        char what[80];
        snprintf(what, sizeof(what), "thermalmonitord has read %u of the %u sensors it reads",
                 static_cast<unsigned>(__builtin_popcountll(served)),
                 static_cast<unsigned>(__builtin_popcountll(kReadMask)));
        add(what);
    }
    for (unsigned s = 0; s < SWITCH_SRC_COUNT; ++s) {
        int32_t milliC = 0;
        uint64_t at = 0;
        if (!read(static_cast<switch_sensor_source>(s), &milliC, &at))
            add(kSourceNames[s]);
    }
    const OSObject *probe = switch_hid_temperature_event(mach_absolute_time(), 0);
    if (!probe)
        add("temperature events");
    OSSafeReleaseNULL(probe);
    return why ? !why[0] : false;
}
}  // namespace

extern "C" void switch_sensors_note(void)
{
    IOLock *lock = __atomic_load_n(&gLock, __ATOMIC_ACQUIRE);
    if (!lock)
        return;
    IOLockLock(lock);
    IOLockWakeup(lock, &gEvent, false);
    IOLockUnlock(lock);
}

extern "C" int switch_sensors_wait_ready(unsigned int wait_ms, char *why, unsigned long len)
{
    char missing[256] = "";
    IOLock *lock = sensorsLock();
    if (!lock) {
        snprintf(why, len, "no memory for the sensors' lock");
        return 0;
    }
    uint64_t deadline = 0;
    clock_interval_to_deadline(wait_ms, kMillisecondScale, &deadline);
    IOLockLock(lock);
    bool ok = ready(missing, sizeof(missing));
    while (!ok && mach_absolute_time() < deadline) {
        // Woken by a source that changed (switch_sensors_note), or at the deadline.
        IOLockSleepDeadline(lock, &gEvent, deadline, THREAD_UNINT);
        ok = ready(missing, sizeof(missing));
    }
    IOLockUnlock(lock);
    snprintf(why, len, "%s", missing);
    int32_t v[SWITCH_SRC_COUNT] = {};
    uint32_t down = 0;
    for (unsigned s = 0; s < SWITCH_SRC_COUNT; ++s) {
        uint64_t at = 0;
        if (!read((switch_sensor_source)s, &v[s], &at))
            down |= 1u << s;
    }
    __atomic_store_n(&gSourceDown, down, __ATOMIC_RELAXED);  // from here on, one line per source that changes
    if (ok && !__atomic_load_n(&gVerdictLogged, __ATOMIC_RELAXED)) {
        char line[160];
        size_t n = 0;
        static const char *const kLabel[] = {"CPU", "", "", "", ", GPU", ", PLLX", ", SoC diode", ", PCB", ", battery"};
        for (unsigned s = 0; s <= SWITCH_SRC_BATTERY && n < sizeof(line); ++s) {
            const int32_t a = v[s] < 0 ? -v[s] : v[s];
            n += (size_t)snprintf(line + n, sizeof(line) - n, "%s %s%d.%d", kLabel[s], v[s] < 0 ? "-" : "", a / 1000,
                                  a % 1000 / 100);
        }
        IOLog("SwitchSensors: thermalmonitord's %u sensors answer (it has read all %u it reads): %s C\n",
              SWITCH_SENSOR_COUNT, (unsigned)__builtin_popcountll(kReadMask), line);
    }
    __atomic_store_n(&gVerdictLogged, true, __ATOMIC_RELEASE);
    return static_cast<int>(ok);
}

extern "C" int switch_sensors_start(void)
{
    if (gStarted)
        return 0;
    gStarted = true;
    if (!sensorsLock()) {
        IOLog("SwitchSensors: no lock\n");
        return -1;
    }
    (void)socThermStart();  // without it the six SOC_THERM sources fail their reads (one line said why)
    unsigned published = 0;
    for (unsigned i = 0; i < SWITCH_SENSOR_COUNT; ++i) {
        const switch_sensor &sensor = switch_sensors[i];
        // NOLINTNEXTLINE(misc-const-correctness): __atomic_store_n publishes it in gDevices, which are not const
        IOService *device = switch_hid_create_temperature(sensor.matchName, switch_sensor_fourcc(sensor.fourcc),
                                                          &answer, (void *)(uintptr_t)i);
        __atomic_store_n(&gDevices[i], device, __ATOMIC_RELEASE);  // ready() reads them from the waiter's thread
        if (device)
            ++published;
    }
    switch_sensors_note();  // a waiting watchdogd re-checks now that the services exist
    (void)switch_virtual_temps_start();
    if (published != SWITCH_SENSOR_COUNT)
        IOLog("SwitchSensors: %u of %u thermalmonitord sensors published\n", published, SWITCH_SENSOR_COUNT);
    if (!published)
        return -1;
    // The event drivers match asynchronously; hookPass waits for them off the board thread.
    gHookCall =
        thread_call_allocate_with_options(&hookPass, nullptr, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    if (gHookCall) {
        (void)thread_call_enter(gHookCall);
    } else {
        IOLog("SwitchSensors: no thread call for the hook pass: the publish notification and the verdict hook\n");
    }
    return 0;
}

extern "C" void switch_sensors_resume(void)
{
    if (!gStarted || !__atomic_load_n(&gSocTherm, __ATOMIC_ACQUIRE))
        return;
    __atomic_store_n(&gSocTherm, (volatile uint8_t *)nullptr, __ATOMIC_RELEASE);  // reads fail until reprogrammed
    switch_soctherm_armed(nullptr);
    (void)socThermStart();
}
