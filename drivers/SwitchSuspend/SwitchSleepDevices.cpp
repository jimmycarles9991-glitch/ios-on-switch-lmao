// SPDX-License-Identifier: BSD-2-Clause-Patent
// Every driver's sleep and wake hook, in order (SwitchSleepDevices.h, SwitchSuspend.h).
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include "../SwitchBattery/SwitchBattery.h"
#include "../SwitchBluetooth/SwitchBluetooth.h"
#include "../SwitchMotion/SwitchMotion.h"
#include "../SwitchUSB/SwitchUSB.h"
#include "../SwitchWLAN/SwitchWLANStation.h"
#include "../TegraGM20B/TegraGM20BSleep.h"
#include "../TegraHost1x/TegraHost1x.h"
#include "../TegraSE/TegraSE.h"
#include "../../diagnostics/switch/SwitchPowerTrail.h"
#include "SwitchSleepDevices.h"
#include "SwitchSleepSteps.h"
#include "SwitchWake.h"

// The hooks (strong: the kernel link takes no undefined weak references; every one below is in a linked driver).
extern "C" {
void switch_block_suspend(void);
void switch_block_resume(void);
void switch_thermal_suspend(void);
void switch_thermal_resume(void);
void switch_sensors_resume(void);
void switch_rtc_suspend(void);
void switch_touch_suspend(void);
void switch_touch_resume(void);
void switch_als_suspend(void);
void switch_als_resume(void);
// Owned by the display and controllers agents (NativeMobileDisplay/TegraDisplayPower.h, SwitchJoyCon/SwitchJoyCon.h).
void tegra_display_suspend(void);
void tegra_display_resume(void);
void switch_joycon_suspend(void);
void switch_joycon_resume(void);
void switch_audio_suspend(void);  // SwitchAudio
void switch_audio_resume(void);
}

#if DEVELOPMENT || DEBUG
extern "C" void switch_sleep_mark(unsigned step)
{
    switch_power_trail_mark(step);
    switch_power_trail_mark_flush();
}
#endif

void switch_sleep_timed(uint8_t step, void (*hook)())
{
    switch_sleep_mark(step);
    const uint64_t start = mach_absolute_time();
    hook();
    const uint64_t us = switch_ns_since(start) / 1000;
    switch_power_journal("SwitchSuspend: %s: %llu.%03llu ms", switch_sleep_step_name(step), us / 1000, us % 1000);
}

bool switch_sleep_devices()
{
    // first: it shares LDO6 with the touch panel, and its clock is the display's frame end
    switch_sleep_timed(SWITCH_STEP_SLEEP_ALS, switch_als_suspend);
    switch_sleep_timed(SWITCH_STEP_SLEEP_DISPLAY, tegra_display_suspend);
    switch_sleep_timed(SWITCH_STEP_SLEEP_GPU, tegra_gm20b_suspend);
    switch_sleep_timed(SWITCH_STEP_SLEEP_TOUCH, switch_touch_suspend);
    switch_sleep_timed(SWITCH_STEP_SLEEP_MOTION, switch_motion_suspend);  // IMU off, SPI4 clock off
    switch_sleep_timed(SWITCH_STEP_SLEEP_JOYCON, switch_joycon_suspend);
    switch_sleep_timed(SWITCH_STEP_SLEEP_USB, switch_usb_suspend);
    switch_sleep_mark(SWITCH_STEP_SLEEP_BLUETOOTH);
    const uint64_t at = mach_absolute_time();
    const bool btWake = switch_bluetooth_suspend();
    switch_power_journal("SwitchSuspend: sleep bluetooth: %llu us", switch_ns_since(at) / 1000);
    switch_sleep_timed(SWITCH_STEP_SLEEP_WIFI, switch_wlan_suspend);
    switch_sleep_timed(SWITCH_STEP_SLEEP_HOST1X,
                       tegra_nvdec_suspend);  // NVDEC, NVENC, VIC, NVJPG quiesced, then host1x
    switch_sleep_timed(SWITCH_STEP_SLEEP_AUDIO, switch_audio_suspend);
    switch_sleep_timed(SWITCH_STEP_SLEEP_THERMAL, switch_thermal_suspend);
    switch_sleep_timed(SWITCH_STEP_SLEEP_SE, tegra_se_suspend);  // idle for BL31's context save at SYSTEM_SUSPEND
    switch_sleep_timed(SWITCH_STEP_SLEEP_NVRAM, [] {
        PESyncNVRAM();
    });  // the NVRAM controller's sync (SwitchNVRAM), SD still up
    switch_sleep_timed(SWITCH_STEP_SLEEP_RTC, switch_rtc_suspend);
    switch_sleep_timed(SWITCH_STEP_SLEEP_BATTERY, switch_battery_suspend);  // the gauge's alert: charge-zone edges only
    return btWake;
}

void switch_sleep_storage()
{
    // The journal to Data while the SD is up: what follows is in RAM only until the wake (and lost to a forced
    // power-off).
    switch_sleep_timed(SWITCH_STEP_SLEEP_JOURNAL, [] {
        switch_power_journal_flush(true);
    });
    switch_sleep_timed(SWITCH_STEP_SLEEP_SD, switch_block_suspend);
}

void switch_wake_devices()
{
    switch_sleep_timed(SWITCH_STEP_WAKE_SE, tegra_se_resume);
    switch_sleep_timed(SWITCH_STEP_WAKE_SD, switch_block_resume);
    switch_sleep_timed(SWITCH_STEP_WAKE_BATTERY, switch_battery_resume);  // the normal gauge windows, a re-read
    switch_sleep_timed(SWITCH_STEP_WAKE_THERMAL, switch_thermal_resume);
    switch_sleep_timed(SWITCH_STEP_WAKE_SENSORS, switch_sensors_resume);
    switch_sleep_timed(SWITCH_STEP_WAKE_HOST1X, tegra_nvdec_resume);  // before the display, which sits behind host1x
    switch_sleep_timed(SWITCH_STEP_WAKE_GPU,
                       tegra_gm20b_resume);  // before the display: its frames go through the GPU present
    switch_sleep_timed(SWITCH_STEP_WAKE_AUDIO, switch_audio_resume);
    switch_sleep_timed(SWITCH_STEP_WAKE_WIFI, switch_wlan_resume);  // the bring-up runs on SwitchWLAN's worker
    switch_sleep_timed(SWITCH_STEP_WAKE_BLUETOOTH, switch_bluetooth_resume);
    switch_sleep_timed(SWITCH_STEP_WAKE_USB, switch_usb_resume);
    switch_sleep_timed(SWITCH_STEP_WAKE_JOYCON, switch_joycon_resume);
    switch_sleep_timed(SWITCH_STEP_WAKE_MOTION, switch_motion_resume);
    switch_sleep_timed(SWITCH_STEP_WAKE_TOUCH, switch_touch_resume);
    switch_sleep_timed(SWITCH_STEP_WAKE_DISPLAY, tegra_display_resume);
    switch_sleep_timed(SWITCH_STEP_WAKE_ALS,
                       switch_als_resume);  // last: after LDO6 (touch) and the frame ends (display) are back
}
