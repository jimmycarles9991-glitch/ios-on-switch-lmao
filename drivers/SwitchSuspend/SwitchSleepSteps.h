// SPDX-License-Identifier: BSD-2-Clause-Patent
// Every step of a system sleep and its wake, as one power-trail code (diagnostics/switch/SwitchPowerTrail.h: the
// durable mark's sleep, SC7 entry, SC7 exit and wake phases), in the order they run. Debug kernels write each before
// its step runs, so the step a hang stopped in is the last one written:
//   * switch_sleep_mark while TegraI2C is up (thread context): the durable mark, which a forced power-off keeps, in
//     the RTC before the step runs;
//   * switch_power_trail_mark_now for the SC7 entry and the first steps out of it, which run with interrupts off on
//     the boot CPU alone (SWITCH_STEP_SYSTEM_SUSPEND, SWITCH_STEP_RESUME);
//   * a trail line only between those, where TegraI2C is down and other cores may run.
// Plain C, for the trail's decoder and the host tests.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Inside the sleep action's hooks (0x01-0x0f): the watchdog armed, then the display's and the GPU's own steps.
enum {
    SWITCH_STEP_SLEEP_WATCHDOG = 0x01,
    SWITCH_STEP_DISPLAY_QUIESCED,
    SWITCH_STEP_DISPLAY_DC_OFF,
    SWITCH_STEP_DISPLAY_DSI_OFF,
    SWITCH_STEP_DISPLAY_CLOCKS_OFF,
    SWITCH_STEP_GPU_GATE,
    SWITCH_STEP_GPU_FENCES,
    SWITCH_STEP_GPU_IDLE_OFF,
    SWITCH_STEP_GPU_NVKM,
    SWITCH_STEP_GPU_POWER_DOWN,
    SWITCH_STEP_GPU_DOWN,
};

// Sleep action (SwitchSuspend::sleepDevices): thread context, every core up.
enum {
    SWITCH_STEP_SLEEP = 0x20,
    SWITCH_STEP_SLEEP_ALS,
    SWITCH_STEP_SLEEP_DISPLAY,
    SWITCH_STEP_SLEEP_GPU,
    SWITCH_STEP_SLEEP_TOUCH,
    SWITCH_STEP_SLEEP_MOTION,
    SWITCH_STEP_SLEEP_JOYCON,
    SWITCH_STEP_SLEEP_USB,
    SWITCH_STEP_SLEEP_BLUETOOTH,
    SWITCH_STEP_SLEEP_WIFI,
    SWITCH_STEP_SLEEP_HOST1X,
    SWITCH_STEP_SLEEP_AUDIO,
    SWITCH_STEP_SLEEP_THERMAL,
    SWITCH_STEP_SLEEP_SE,
    SWITCH_STEP_SLEEP_NVRAM,
    SWITCH_STEP_SLEEP_RTC,
};

// SC7 entry: the sleep action's last steps (the wake events and the BPMP set up, the journal written, the SD down),
// then IOCPUSleepKernel (ArmCpu): the secondaries, the boot CPU with interrupts off.
enum {
    SWITCH_STEP_SLEEP_BATTERY = 0x30,
    SWITCH_STEP_SLEEP_PMC_WAKE,
    SWITCH_STEP_SLEEP_BPMP,
    SWITCH_STEP_SLEEP_JOURNAL,
    SWITCH_STEP_SLEEP_SD,
    SWITCH_STEP_SLEEP_I2C,
    SWITCH_STEP_HALT_CPU1,
    SWITCH_STEP_HALT_CPU2,
    SWITCH_STEP_HALT_CPU3,
    SWITCH_STEP_HALT_BOOT_CPU,
    SWITCH_STEP_QUIESCE,
    SWITCH_STEP_PSCI_PEERS_OFF,
    SWITCH_STEP_SYSTEM_SUSPEND,
};

// SC7 exit: the boot CPU with interrupts off (XNU's arm_init_cpu, the first C to run; ArmCpu), then the wake action's
// first steps (SwitchSuspend::wakeDevices, every core up again).
enum {
    SWITCH_STEP_RESUME = 0x40,
    SWITCH_STEP_RESUME_CPU,
    SWITCH_STEP_RESUME_ACTIVE,
    SWITCH_STEP_WAKE,
    SWITCH_STEP_WAKE_I2C,
    SWITCH_STEP_WAKE_BPMP,
    SWITCH_STEP_WAKE_RTC,
    SWITCH_STEP_WAKE_SMMU,
    SWITCH_STEP_WAKE_SE,
    SWITCH_STEP_WAKE_SD,
};

// The rest of the wake action.
enum {
    SWITCH_STEP_WAKE_BATTERY = 0x50,
    SWITCH_STEP_WAKE_THERMAL,
    SWITCH_STEP_WAKE_SENSORS,
    SWITCH_STEP_WAKE_HOST1X,
    SWITCH_STEP_WAKE_GPU,
    SWITCH_STEP_WAKE_AUDIO,
    SWITCH_STEP_WAKE_WIFI,
    SWITCH_STEP_WAKE_BLUETOOTH,
    SWITCH_STEP_WAKE_USB,
    SWITCH_STEP_WAKE_JOYCON,
    SWITCH_STEP_WAKE_MOTION,
    SWITCH_STEP_WAKE_TOUCH,
    SWITCH_STEP_WAKE_DISPLAY,
    SWITCH_STEP_WAKE_ALS,
};

enum {
    SWITCH_STEP_AWAKE = 0x5f,
};

#if DEVELOPMENT || DEBUG
// The durable mark for `step`, in the RTC on return (switch_power_trail_mark and _flush): thread context, TegraI2C up.
void switch_sleep_mark(unsigned step);
#else
#define switch_sleep_mark(step) ((void)sizeof(step))
#endif

// The trail line alone (SwitchPowerTrail.h included).
#define switch_sleep_trail(step) switch_power_trail("step %#04x %s", (unsigned)(step), switch_sleep_step_name(step))

// A step's name, or 0 for a code that is none.
// NOLINTBEGIN(modernize-use-nullptr): C (TegraGM20B's gpu_*.c) reads this header too
static inline const char *switch_sleep_step_name(unsigned step)
{
    static const char *const names[0x40] = {
        "sleep",
        "sleep als",
        "sleep display",
        "sleep gpu",
        "sleep touch",
        "sleep motion",
        "sleep joycon",
        "sleep usb",
        "sleep bluetooth",
        "sleep wifi",
        "sleep host1x",
        "sleep audio",
        "sleep thermal",
        "sleep se",
        "sleep nvram",
        "sleep rtc",
        "sleep battery",
        "sleep pmc wake",
        "sleep bpmp",
        "sleep journal",
        "sleep sd",
        "sleep i2c",
        "halt cpu1",
        "halt cpu2",
        "halt cpu3",
        "halt boot cpu",
        "quiesce",
        "psci peers off",
        "SYSTEM_SUSPEND",
        0,
        0,
        0,
        "resume",
        "resume cpu",
        "resume active",
        "wake",
        "wake i2c",
        "wake bpmp",
        "wake rtc",
        "wake smmu",
        "wake se",
        "wake sd",
        0,
        0,
        0,
        0,
        0,
        0,
        "wake battery",
        "wake thermal",
        "wake sensors",
        "wake host1x",
        "wake gpu",
        "wake audio",
        "wake wifi",
        "wake bluetooth",
        "wake usb",
        "wake joycon",
        "wake motion",
        "wake touch",
        "wake display",
        "wake als",
        0,
        "awake",
    };
    static const char *const inner[] = {
        "sleep watchdog armed", "display quiesced",   "display DC off",
        "display DSI off",      "display clocks off", "gpu gate",
        "gpu fences",           "gpu idle off",       "gpu nvkm",
        "gpu power down",       "gpu down",
    };
    if (step >= SWITCH_STEP_SLEEP_WATCHDOG && step < SWITCH_STEP_SLEEP_WATCHDOG + sizeof(inner) / sizeof(inner[0])) {
        // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound): the test above keeps step inside inner
        return inner[step - SWITCH_STEP_SLEEP_WATCHDOG];
    }
    return step >= SWITCH_STEP_SLEEP && step < SWITCH_STEP_SLEEP + 0x40u ? names[step - SWITCH_STEP_SLEEP] : 0;
}

// NOLINTEND(modernize-use-nullptr)

#ifdef __cplusplus
}
#endif
