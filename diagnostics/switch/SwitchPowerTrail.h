// SPDX-License-Identifier: BSD-2-Clause-Patent
// The power trail: what a shut down, a restart, a system sleep or a wake got to, kept where a hang leaves it. Debug
// kernels only; in release every call compiles to nothing (arguments type-checked, never evaluated).
//
// Two stores, because a hang usually ends with POWER held, and a MAX77620 power-off loses DRAM and the PMC scratch
// registers alike (VDD_RTC is the PMIC's LDO4, off with the rest):
//   * lines (switch_power_trail): text, in 16 KiB of SwitchPanicSave.c's reserved RAM (0xb017c000, the last 255
//     lines), written through and drained at every line. A warm reset keeps them (the SoC watchdog, a panic's PMC main
//     reset), and so does SC7, where DRAM self-refreshes; POWER held does not. switch_power_trail_watch turns a hang
//     into a warm reset, unless POWER is held first;
//   * the durable mark (switch_power_trail_mark*): one code byte in the MAX77620's battery-backed RTC
//     (drivers/SwitchPower/SwitchPowerMark.cpp), which only a flat battery clears.
// The next boot logs the last boot's lines ("SwitchPowerTrail: last boot: ...", from kern_exec.c before launchd) and
// its last mark ("SwitchPowerTrail: durable mark: ...", once the PMIC answers on I2C5), both to the unified log, where
// `idevicesyslog archive` gets them over USB. Each line also goes to os_log as it is written, when interrupts are on.
#ifndef SWITCH_POWER_TRAIL_H
#define SWITCH_POWER_TRAIL_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Durable mark codes, 0x00..0x7f: the high nibble is the phase, a phase's 0x?f its normal end.
enum {
    SWITCH_MARK_NONE = 0x00,
    SWITCH_MARK_BOOT_PMIC = 0x11,       // the first mark a boot can write: I2C5 and the PMIC answer
    SWITCH_MARK_BOOT_ROOT = 0x12,       // the root file system mounted
    SWITCH_MARK_BOOT_LAUNCHD = 0x13,    // launchd about to start
    SWITCH_MARK_WATCHDOG_BENCH = 0x1e,  // the SoC watchdog armed by hand and never petted (debug.switch_watchdog_bench)
    SWITCH_MARK_BOOT_DONE = 0x1f,       // IOPMrootDomain's System Boot Complete
    SWITCH_MARK_SLEEP = 0x20,           // system sleep, thread context steps (SwitchSuspend)
    SWITCH_MARK_SC7 = 0x30,             // SC7 entry, interrupts off
    SWITCH_MARK_RESUME = 0x40,          // SC7 exit, interrupts off
    SWITCH_MARK_WAKE = 0x50,            // wake, thread context steps; 0x5f wake complete
    SWITCH_MARK_HALT_BEGAN = 0x60,      // reboot(2): shut down or restart, before the SoC watchdog is armed
    SWITCH_MARK_HALT_NOTIFIED = 0x61,   // IOPMRootDomainWillShutdown and paging off: NVRAM written
    SWITCH_MARK_HALT_PROCS = 0x62,      // proc_shutdown, every process but driver extensions
    SWITCH_MARK_HALT_OTHER = 0x63,      // audit_shutdown, shutdown.trace or any other IOKit halt step
    SWITCH_MARK_HALT_UNMOUNTED = 0x64,  // vfs_unmountall, the first
    SWITCH_MARK_HALT_PROCS_ALL = 0x65,  // proc_shutdown, the rest
    SWITCH_MARK_HALT_UNMOUNTED_ALL = 0x66,  // vfs_unmountall, the second
    SWITCH_MARK_HALT_BUFFERS = 0x67,        // the buffer cache clean
    SWITCH_MARK_HALT_INTERFACES = 0x68,     // if_down_all
    SWITCH_MARK_HALT_DRIVER_SLOW = 0x69,    // a driver's systemWillShutdown still running after 3 s
    SWITCH_MARK_HALT_QUIESCED = 0x6a,       // the power tree quiesced
    SWITCH_MARK_HALT_DRIVERS = 0x6b,        // every driver told (IOPMrootDomain "PowerOff" / "Restart")
    SWITCH_MARK_HALT_PLATFORM = 0x6c,       // PE_halt_restart
    SWITCH_MARK_HALT_CPU = 0x6d,            // the CPU off the DFLL
    SWITCH_MARK_HALT_PMIC = 0x6e,     // the panel dark, the MAX77620 soft reset next: what a power-off that took leaves
    SWITCH_MARK_HALT_NOT_OFF = 0x6f,  // the soft reset written, the console still running 2 s later
};

// Flags on the mark in the RTC, set with interrupts off, keeping the code of the step that was running.
enum {
    SWITCH_MARK_FLAG_DEADLINE = 1u << 0,     // the shutdown deadline passed
    SWITCH_MARK_FLAG_PANIC_RESET = 1u << 1,  // a panic's warm reset (SwitchPanicSave.c), set just before it
};

#if DEVELOPMENT || DEBUG
// One line, printf format: its first 47 characters to the trail, 159 to os_log. Any context but an FIQ, interrupts on
// or off; never blocks.
void switch_power_trail(const char *format, ...) __attribute__((format(printf, 1, 2)));

// The SoC watchdog (TMR5/WDT0, PMC2CAR: a warm reset, DRAM kept) armed to reset the console `seconds` (at most 255)
// from now unless called again; 0 disarms it. One trail line names `why`. Any context; never blocks. A panic re-arms it
// for its own reset. Disarm before SC7 entry and re-arm on the way out: the timer block loses power in SC7.
void switch_power_trail_watch(unsigned seconds, const char *why);

// A durable mark and a trail line. Any context; never blocks: a kernel thread call writes it over I2C5 (TegraI2C), and
// marks that come faster than the RTC takes them (one update per 16 ms) give way to the latest. Nothing reaches the RTC
// before the PMIC driver has started (the first boot mark) or while TegraI2C is suspended.
void switch_power_trail_mark(unsigned code);
// Thread context: returns once the last mark is in the RTC, about 1 ms of I2C plus up to 16 ms for the RTC.
void switch_power_trail_mark_flush(void);
// A mark written now, for code that runs with interrupts off and nothing else running (SC7 entry and exit, TegraI2C
// suspended or not yet resumed): polled I2C5 (tegra_i2c_raw_write), about 0.5 ms, after up to 16 ms spun out while the
// RTC finishes the previous update. Not while a thread-context mark is being written.
void switch_power_trail_mark_now(unsigned code);
// A SWITCH_MARK_FLAG_* on the mark in the RTC, as mark_now writes (and in the same contexts); the code stays.
void switch_power_trail_flag_now(unsigned flag);

// SwitchPanicSave.c: the region's trail slice, at its init (the last boot's lines are read before any is written);
// and the last boot's lines to os_log, once Data is mounted. SwitchPowerMark.cpp: the mark the last boot left (-1:
// none readable), how many that boot wrote and its flags, logged at once.
void switch_power_trail_init(volatile uint8_t *slice, uint32_t bytes);
void switch_power_trail_report(void);
void switch_power_trail_durable(int code, unsigned count, unsigned flags);
// The RTC's writer behind the marks (SwitchPowerMark.cpp), as the calls above describe.
void switch_pmic_mark(uint8_t code);
void switch_pmic_mark_flush(void);
void switch_pmic_mark_now(uint8_t code);
void switch_pmic_flag_now(uint8_t flag);
#else
int switch_power_trail_unused(const char *format, ...) __attribute__((format(printf, 1, 2)));
#define switch_power_trail(...)                        ((void)sizeof(switch_power_trail_unused(__VA_ARGS__)))
#define switch_power_trail_watch(seconds, why)         ((void)sizeof((seconds) + sizeof(why)))
#define switch_power_trail_mark(code)                  ((void)sizeof(code))
#define switch_power_trail_mark_flush()                ((void)0)
#define switch_power_trail_mark_now(code)              ((void)sizeof(code))
#define switch_power_trail_flag_now(flag)              ((void)sizeof(flag))
#define switch_power_trail_init(slice, bytes)          ((void)0)
#define switch_power_trail_report()                    ((void)0)
#define switch_power_trail_durable(code, count, flags) ((void)0)
#endif

#ifdef __cplusplus
}
#endif
#endif
