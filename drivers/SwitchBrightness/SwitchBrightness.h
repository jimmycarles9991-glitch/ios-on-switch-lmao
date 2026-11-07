// SPDX-License-Identifier: BSD-2-Clause-Patent
// The built-in display's backlight, display power and ambient light sensor for iPadOS 17 CoreBrightness
// (SwitchBacklight.cpp, SwitchALS.cpp; the panel itself is TegraPanel.h).
//
// CoreBrightness's brightness reaches the panel (DCS 0x51). Display power: the render server's power requests (lock,
// auto-lock, wake) turn the panel and the touch controller off and on and stop and restart the display controller's
// scanout; system sleep (drivers/SwitchSuspend) needs it, and boot-arg switch-no-sleep turns it off. The BH1730FVC is
// CoreBrightness's ALS (HID service 0xff00/0x0004), clocked by display controller A's frame ends (SwitchALS.cpp).
#ifndef SWITCH_BRIGHTNESS_H
#define SWITCH_BRIGHTNESS_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

// Publishes the backlight service. Needs the original AppleARMPlatform kext loaded (it is a dependency of
// IOMobileGraphicsFamily, so call this after that family has loaded, e.g. at the end of the display probe). Thread
// context, idempotent; 0 on success, -1 if the original AppleARMBacklight class is not registered at the bound address
// or setup failed.
int switch_backlight_start(void);

// Whether display power is on for this boot (no switch-no-sleep). Any context.
bool switch_display_power_enabled(void);

// Display power, from the framebuffer when the render server asks for it (IOMobileFramebufferRequestPowerChange: 0 off,
// anything else on). Returns false (and does nothing) unless display power is on and the backlight has started; the caller
// then keeps the original behaviour. Never blocks: the panel sequence (about 0.3 s each way) runs on the backlight's
// work loop, which calls native_legacy_display_set_scanout() below in the right order. Any thread context.
bool switch_display_power_request(bool on);

// False from the moment a power-off request is taken until the panel is lit again; for input drivers that must ignore
// the screen while it is dark. Lock-free, any context.
bool switch_display_is_on(void);

// Implemented by the display driver (NativeLegacyDisplay), weak here: stops (false) or restarts (true) display
// controller A's scanout and masks or unmasks its frame-end and vblank interrupts, under the display's work loop; returns
// true when done. Called from the backlight's work loop only: after the panel is off, and before it is turned on again.
bool native_legacy_display_set_scanout(bool on);

// Starts the ambient light sensor. Needs TegraI2C, the GIC and
// IOHIDFamily (switch_hid_create loads it); thread context, may sleep. Publishes the HID service at once and talks to the
// sensor at its first sample after LDO6 is on (switch_als_supply). A sensor that does not answer, or no LDO6 within
// 20 s, logs one line and leaves it off; a lost display frame end panics. 0 on success.
int switch_als_start(void);

// SwitchTouch, on its work loop: LDO6 (the touch panel's 2.9 V, also the sensor's I2C reference and, by Horizon's
// I2C2 bus regulator, the bus's pull-ups) is on and has been for 10 ms or more (true), or is about to go off (false).
// Before switch_als_start() the state is kept for it. Thread context.
void switch_als_supply(bool on);

// True once the ALS HID service is registered: MobileGestalt's ambient-light-sensor is then true (XNU's kern_exec drops
// MobileGestalt's cache when this differs from the last boot's). Any context.
bool switch_als_published(void);

// The backlight tells the sensor when the panel goes dark or lights up: the sensor stops converting while it is dark.
// Thread context.
void switch_als_set_active(bool active);

// System sleep (SwitchSuspend), thread context, I2C up. Suspend before the display and touch suspend (the sensor shares
// LDO6 with the touch panel, and its clock is the display's frame end); resume after both have resumed (the sensor is
// configured again at its first sample, which needs LDO6 and frame ends).
void switch_als_suspend(void);
void switch_als_resume(void);

#ifdef __cplusplus
}
#endif
#endif
