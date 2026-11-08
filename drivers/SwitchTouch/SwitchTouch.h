// SPDX-License-Identifier: BSD-2-Clause-Patent
// Nintendo Switch OLED touch screen (STMicro FingerTip S FTM4 on I2C3 0x49) as an iPadOS multitouch digitizer.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Creates the touch HID device (loads IOHIDFamily on first use) and the driver, whose work loop then powers the
// panel up and takes its interrupt. Thread context, after bsd_autoconf() (IOKit, OSKext and the GIC up). Idempotent.
// Returns 0. A panel that does not come up is one log line, touch off until the screen next lights.
int switch_touch_start(void);

// System sleep (drivers/SwitchSuspend), thread context with I2C up: panel off before SC7, brought up again after it
// unless the screen is dark. No-ops before switch_touch_start().
void switch_touch_suspend(void);
void switch_touch_resume(void);

// Display power (SwitchBrightness, lock and wake), thread context: the touch panel powered off with the screen (reset low,
// LDO6 off, its interrupt and watchdog stopped) and brought up again, asynchronously, when the screen lights. LDO6 also
// powers the ALS: the caller stops it first (switch_als_set_active). Returns at once when on; no-op before
// switch_touch_start().
void switch_touch_display_power(bool on);

#ifdef __cplusplus
}
#endif
