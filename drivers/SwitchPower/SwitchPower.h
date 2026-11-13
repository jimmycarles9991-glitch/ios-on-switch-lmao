// SPDX-License-Identifier: BSD-2-Clause-Patent
// Nintendo Switch OLED (T210B01) power services, statically linked into the kernel.
#ifndef SWITCH_POWER_H
#define SWITCH_POWER_H
#ifdef __cplusplus
extern "C" {
#endif

// Starts the thermal service (SwitchThermal.cpp), its single entry point: drives the fan to a fixed safe duty at once
// (MMIO only), claims the TMP451 ALERT interrupt (GPIO PX4, level low; panics if it cannot be had), then follows the
// L4T fan curve on every ALERT, with a re-read every 5 s as a staleness bound. It also raises the CPU (the DFLL on
// demand, or up a ladder of L4T rates on PLLX with switch-no-dfll or after a DFLL failure) while the SoC is below its thermal trip,
// dropping to the inherited clocks at the trip and going up again below the hysteresis, at most once per hold-off.
// switch_pmic_start() calls it before the root mount; the board thread's later call is a no-op then. Thread context
// after IOKit is up; idempotent.
// Returns 0 on success, -1 if the service could not be created (the fan may still be running at the safe duty if
// the MMIO part succeeded).
int switch_thermal_start(void);

// With the CPU on the DFLL, moves it back to PLLX so that SwitchPMIC's
// shut-down/restart I2C5 traffic has the bus to itself; nothing otherwise. Thread context; waits for the thermal work
// loop.
void switch_thermal_cpu_quiesce(void);

// Performance hints for the CPU's on-demand governor (SwitchPerfGovernor.cpp): the CPU's top operating point at once,
// held for a while after the last hint (BOOT_DONE instead ends the boot window: the CPU's floor at its ceiling and
// the GPU's idle-clock hold, once per boot, one line). The GPU has no load governor: it
// runs its full clock whenever it has work, and a touch, missed frame or launch brings it back at once from its idle
// clock (TegraGM20B gpu_clock.c, entered only after 500 ms with no submission and no swap). Any context: thread, work loop,
// primary interrupt, the scheduler's; a few stores under a spin lock, nothing when the governor is off (BOOT_DONE:
// thread context, it logs). Callers: SwitchTouch (each touch interrupt), NativeLegacyDisplay (a present two or more
// refresh periods after the last one, while frames keep coming), XNU exec_activate_image (LAUNCH) and IOPMrootDomain at
// SpringBoard's System Boot Complete (BOOT_DONE, whether or not the boot band's 60 s bound closed the band first).
enum {
    SWITCH_PERF_HINT_TOUCH = 1,
    SWITCH_PERF_HINT_FRAME_MISS = 2,
    SWITCH_PERF_HINT_LAUNCH = 3,
    SWITCH_PERF_HINT_BOOT_DONE = 4,
};

void switch_perf_hint(unsigned int hint);

// The CPU clock the governor last applied, in kHz; 0 when the CPU is not governed (PLL or fixed DFLL mode). Any
// context (one atomic load): kprof prints it next to its measured per-core clock.
unsigned int switch_perf_cpu_khz(void);

// System Boot Complete has come (or the boot window's fallback passed). Any context (one atomic load).
bool switch_perf_boot_done(void);

// The thermal service's last good TMP451 reading: the SoC diode and the PCB (m C) and when it was taken (mach absolute
// time). 0 before the first one or after the sensor was lost. Any context.
int switch_thermal_tmp451(int *soc_millic, int *pcb_millic, unsigned long long *at);

// iPadOS's CPU ceiling (thermalmonitord's DVD factor on ApplePMGR, a 16.16 share of the top clock; 0x10000 = none,
// kits/qemu/ShengPlatformPower.cpp): the CPU governor's highest point is also at or below it, at once. Kept across
// governor restarts. Thread context (thermalmonitord's property write). Returns the governor's ceiling now in kHz, 0
// when the CPU is not governed (PLL or fixed DFLL mode: the ceiling has no effect then).
unsigned int switch_perf_cltm_ceiling(unsigned int fraction16);

// thermalmonitord's HID temperature sensors (SwitchSensors.cpp, CONTRACT.md): SOC_THERM's six on-die sensors brought
// up, then one HID service per sensor the J120AP thermal model expects, each answering from the Switch sensor nearest
// to what it measures. Board thread, after SwitchThermal and SwitchBattery; 0, or -1 (one log line) when nothing could
// be published (thermalmonitord then stays unhealthy and the userspace watchdog stays off, see below).
int switch_sensors_start(void);
// AppleVTempDispatcher, where thermalmonitord sets its virtual temperatures (SwitchVirtualTemps.cpp).
int switch_virtual_temps_start(void);
// Whether every sensor thermalmonitord checks can answer now: all HID services hooked, SOC_THERM's readings valid, a
// TMP451 and a battery reading. Waits up to wait_ms for that, woken by switch_sensors_note(); false with the missing
// parts in `why` once the time is up. Thread context. NativeSwitchWatchdog asks once, at watchdogd's first open.
int switch_sensors_wait_ready(unsigned int wait_ms, char *why, unsigned long len);
// A source changed state (SwitchThermal's and SwitchBattery's readings, a HID service hooked, a sensor's first read by
// thermalmonitord): wakes the wait above. Any thread context, cheap.
void switch_sensors_note(void);
// After SC7 (SwitchSuspend's wake, thread context): SOC_THERM programmed again (SC7 resets it); its six sources fail
// their reads until then. A no-op when SOC_THERM never came up.
void switch_sensors_resume(void);

// PMIC services (SwitchPMIC.cpp): logs the CPU/EMC clock line, turns on the Wi-Fi/Bluetooth module's 32 kHz sleep
// clock (MAX77620 GPIO4 as 32K_OUT1 and the 32K output enable, as L4T does; Bluetooth needs it), publishes the
// MAX77620 RTC as the "IORTC" resource, installs PE_halt_restart (Shut Down / Restart), sets the EMC clock through the
// BPMP firmware (drivers/TegraBPMP) and logs the clock line again, then starts the thermal service. Called once by
// XNU's IOKitInitializeTime() on the bsd_init thread, before the calendar is read; sleeps on I2C5 and BPMP answers
// and, the first time, until the GIC is up. Returns 0, or -1 when the MAX77620 does not answer (no RTC or power-off
// then; the rest still runs).
int switch_pmic_start(void);

// System sleep hooks (drivers/SwitchSuspend), thread context with I2C up; no-ops when the service did not start.
// Thermal: before SC7 the CPU goes back to the boot chain's clocks and VDD_CPU and the fan stops; after it the fan,
// sensor and CPU operating point come back as at start. RTC: powerd's next scheduled wake ("wake" /
// "WakeByCalendarDate" PM settings) is set as MAX77620 ALARM1 before SC7; after it the wall clock is re-read from the
// RTC and the alarm turned off; switch_rtc_resume returns 1 when ALARM1 is what woke the console.
// switch_rtc_sleep_enable (SwitchSuspend's start, once): take powerd's scheduled wakes from the root domain from now on.
void switch_rtc_sleep_enable(void);
void switch_thermal_suspend(void);
void switch_thermal_resume(void);
void switch_rtc_suspend(void);
int switch_rtc_resume(void);
// The RTC part of the MAX77620 interrupt (SwitchButtons owns the nIRQ): reads RTCINT, which releases the line.
void switch_rtc_interrupt(void);

#ifdef __cplusplus
}
#endif
#endif
