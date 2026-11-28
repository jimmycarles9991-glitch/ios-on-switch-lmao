// SPDX-License-Identifier: BSD-2-Clause-Patent
// Joy-Con HID devices (SwitchJoyConHID.cpp, a late object) for the rail driver (SwitchJoyConRail.cpp).
//
// A rail Joy-Con is presented to iOS exactly as bluetoothd presents a Bluetooth one: an IOHIDDevice with Nintendo's
// VendorID/ProductID (0x057e/0x2006 L, 0x2007 R), the Joy-Con's own 170-byte HID report descriptor, Transport
// "Bluetooth", SerialNumber = the Joy-Con's BD_ADDR, and the Nintendo HID report protocol (output 0x01 subcommands,
// 0x10 rumble; input 0x21 replies, 0x30 full reports, 0x3F simple reports).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <IOKit/IOReturn.h>

class IOService;
class IOWorkLoop;

// Called on the setReport caller's thread with one output report, report ID first.
// Must not block; the rail driver queues it into a lock-free ring.
typedef IOReturn (*SwitchJoyConOutput)(void *context, uint32_t generation, const uint8_t *report, size_t length);

// Called once per device when the backboardd session serving it ends while the device stays (SwitchJoyConSession.h).
// Runs inside IOService::open/close with the device's arbitration lock held; must only record and wake the rail loop.
typedef void (*SwitchJoyConSessionEnded)(void *context, uint32_t generation, int32_t servedPid, int32_t openerPid);

enum : uint32_t {
    kSwitchJoyConLeftPID = 0x2006,
    kSwitchJoyConRightPID = 0x2007
};

void switch_joycon_hid_select_personalities();
void switch_joycon_hid_prepare();

// Creates the shared system-buttons Consumer Control device bound to `loop` once at startup.
void switch_joycon_hid_buttons_init(IOWorkLoop *loop);

// Updates the Home (R) or Capture (L) state on the system-buttons device (call on edges only).
void switch_joycon_hid_buttons_edge(bool left, bool down, uint64_t timestamp);

// Releases the Joy-Con's system button if it was down.
void switch_joycon_hid_buttons_release(bool left);

// Creates, attaches (under IOResources) and starts a Joy-Con device sharing the rail side's `loop`.
IOService *switch_joycon_hid_create(IOWorkLoop *loop, bool left, const uint8_t mac[6], uint32_t generation,
                                    SwitchJoyConOutput output, SwitchJoyConSessionEnded ended, void *context);

// Hands one input report (report ID first) with an IRQ-captured mach_absolute_time timestamp to the device.
// Caller must be on the device's shared rail work loop.
IOReturn switch_joycon_hid_input(const IOService *device, const uint8_t *report, size_t length, uint64_t timestamp);

// Detaches callbacks, terminates the device, and drops the creator's reference. Rail work loop.
void switch_joycon_hid_destroy(const IOService *device);
