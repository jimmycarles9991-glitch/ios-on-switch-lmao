// SPDX-License-Identifier: BSD-2-Clause-Patent
// Nintendo Switch OLED motion sensors (6-axis IMU on SPI4) as iPadOS's accelerometer and gyroscope HID services
// (SwitchMotion.cpp has the hardware and the iPadOS side).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Identifies the IMU, publishes its HID services (loads IOHIDFamily on first use) and waits for clients; the part
// samples only while one has a report interval set. Board thread, after TegraGPIO is up. Idempotent. Returns 0; every
// failure panics with a one-line reason.
int switch_motion_start(void);

// System sleep (drivers/SwitchSuspend's sleep and wake actions; thread context, both from the same thread). Suspend
// stops sampling, powers the part down and gates the SPI4 clock; resume clocks SPI4 again and restores whatever the
// clients had asked for. Neither touches I2C, so their place in the sequence is free. No-ops before
// switch_motion_start().
void switch_motion_suspend(void);
void switch_motion_resume(void);

// Display power (SwitchBacklight, as the panel goes off and comes on; any thread, never blocks): the orientation
// detector runs only while the panel is lit, so with the panel off nothing converts for it. Its first stable
// orientation after the panel comes on is announced. No-op before switch_motion_start().
void switch_motion_display_power(bool on);

// The orientation last announced (0 unknown .. 6 face down, its events' scale) and its sample time (mach time; 0 with
// none yet). Any thread.
void switch_motion_last_orientation(uint8_t *orientation, uint64_t *stamp);

#ifdef __cplusplus
class IOService;
class IOUserClient;
// The orientation SwitchHIDDevice's newUserClient for AppleSPULib's type 'ashd' (SwitchOrientationClient.cpp).
int switch_motion_orientation_client(IOService *device, struct task *task, void *security, uint32_t type,
                                     IOUserClient **handler);
#endif

#ifdef __cplusplus
}
#endif
