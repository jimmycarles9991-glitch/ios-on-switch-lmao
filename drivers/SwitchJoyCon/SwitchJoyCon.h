// SPDX-License-Identifier: BSD-2-Clause-Patent
// Nintendo Switch Joy-Cons as iPadOS game controllers, on the console rails and over Bluetooth, and the
// game-controller personalities every other gamepad (Xbox, DualShock 4, DualSense, generic HID) needs.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Rail Joy-Cons (UART-B right, UART-C left) are Joy-Con HID devices for GameController, with attach/detach detection
// and rail charging (SwitchJoyConRail.cpp); their Home and Capture buttons are also the iPad's Home button and
// screenshot key (SwitchJoyConHID.cpp). The game-controller HID personalities also serve Bluetooth Joy-Cons and Pro
// Controllers and, unless boot-arg switch-no-bt turns Bluetooth off, the rail pairs on attach: once SwitchBluetooth has
// programmed the controller's address (switch_joycon_bluetooth_address), each rail Joy-Con's active pairing slot is
// read, and the Joy-Con is paired to that address over the rail unless the slot already names it; the link key is
// published for joyconpaird to hand to bluetoothd (helper/). On iPadOS 17 our personalities replace the prelinked
// AppleGameControllerPersonality.kext's at start; on iPadOS 27 the kext's own serve (SwitchJoyConHID.cpp).
// Thread context, after bsd_autoconf() with the GIC up (the board thread). Idempotent. Returns 0; failures panic.
int switch_joycon_start(void);

// Rail state across SC7 (thread context, may sleep; both are no-ops unless the rail driver runs).
// switch_joycon_suspend: stops both rails: timers cancelled, detect and UART interrupts masked, rail HID devices
// terminated (GameController sees the Joy-Cons disconnect). The UART, CAR and pinmux state may then be lost.
// switch_joycon_resume: call after SC7 exit once TegraGPIO has restored its pin configuration (the detect pins'
// falling-edge triggers) and the GIC is up: re-initialises pinmux, clocks and UARTs and waits for Joy-Cons again; a
// Joy-Con still seated is found by level and handshaken as on a fresh attach.
void switch_joycon_suspend(void);
void switch_joycon_resume(void);

// Display power (thread context, may sleep; no-ops unless the rail driver runs). Off: both rails stop all traffic (no
// timer runs, the detect interrupt is masked, rail HID devices terminated; charging is left as it is). On: both rails
// wait for Joy-Cons again, and a seated one is handshaken as on a fresh attach. Nothing calls it: SwitchBacklight
// leaves the rails running with the panel off, so a seated Joy-Con (R)'s Home wakes the console as on Horizon
// (../SwitchSuspend/SwitchWakeSources.h); the rails stop only for system sleep (switch_joycon_suspend).
void switch_joycon_display_power(bool on);

// SwitchBluetooth, once it has programmed the controller's BD address (display order) for its first client: the address
// rail Joy-Cons are paired to (SwitchJoyConRail.cpp, "Pairing"). Until then rail Joy-Cons run unpaired; any that are
// running are paired now. Thread context, may sleep; any number of calls (the latest address wins).
void switch_joycon_bluetooth_address(const uint8_t address[6]);

#ifdef __cplusplus
}
#endif
