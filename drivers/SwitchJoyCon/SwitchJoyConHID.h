// SPDX-License-Identifier: BSD-2-Clause-Patent
// Joy-Con HID devices (SwitchJoyConHID.cpp, a late object) for the rail driver (SwitchJoyConRail.cpp).
//
// A rail Joy-Con is presented to iOS exactly as bluetoothd presents a Bluetooth one: an IOHIDDevice with Nintendo's
// VendorID/ProductID (0x057e/0x2006 L, 0x2007 R), the Joy-Con's own 170-byte HID report descriptor, Transport
// "Bluetooth", SerialNumber = the Joy-Con's BD_ADDR, and the Nintendo HID report protocol (output 0x01 subcommands,
// 0x10 rumble; input 0x21 replies, 0x30 full reports, 0x3F simple reports). iPadOS 17 drives that protocol itself:
// /System/Library/HIDPlugins/ServicePlugins/JoyConHIDServicePlugin.plugin (in backboardd's HID event system) matches
// VID 0x057e with PID 0x2006/0x2007/0x2009/0x2017/0x2019, opens the device, sends subcommands with setReport and
// hands the parsed controller to gamecontrollerd, which publishes the GCController. So the kernel only carries
// reports: setReport bytes go to the rail as the Joy-Con's UART encapsulation (cmd 0x92), and every 0x92 reply's
// payload comes back through handleReport. One iOS code path serves the rail and Bluetooth. On iPadOS 27 the device
// exists only while the rail pair is in use, as a Bluetooth one is connected (SwitchJoyConUse.h).
//
// The rail Joy-Cons' Home (R) and Capture (L) buttons are also the iPad's Home button and screenshot key: a built-in
// Consumer Control device (SwitchJoyConButtons, made with the first rail Joy-Con report) reports them as Consumer Menu
// (0x0C/0x40) and Snapshot (0x0C/0x65), which SpringBoard handles itself (SwitchJoyConHID.cpp, "System buttons").
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <IOKit/IOReturn.h>

class IOService;

// Called on the setReport caller's thread (a user client thread) with one output report, report ID first
// (0x01 subcommand: id, packet counter, 8 rumble bytes, subcommand, data; 0x10: id, counter, 8 rumble bytes).
// Must not block for long; the rail driver queues it.
typedef IOReturn (*SwitchJoyConOutput)(void *context, const uint8_t *report, size_t length);

// Called once per device when the backboardd session serving it ends while the device stays (SwitchJoyConSession.h):
// `servedPid` is that backboardd, `openerPid` the different backboardd that opened the device (0: the serving
// client closed it). gamecontrollerd would refuse the device's next plugin session, so the rail publishes a new one.
// Runs on the opener's or closer's thread inside IOService::open/close (the device's arbitration lock held): it
// must only take note and wake the rail's work loop.
typedef void (*SwitchJoyConSessionEnded)(void *context, int32_t servedPid, int32_t openerPid);

enum : uint32_t {
    kSwitchJoyConLeftPID = 0x2006,
    kSwitchJoyConRightPID = 0x2007
};

// Once, before any game controller can appear (switch_joycon_start calls it first): removes
// AppleGameControllerPersonality.kext's personalities if it is prelinked. Thread context.
void switch_joycon_hid_select_personalities();

// Loads IOHIDFamily, registers SwitchJoyConDevice and adds the Nintendo entries of
// AppleGameControllerPersonality.kext (not in the default prelink set): for every Nintendo controller ProductID, an IOHIDEventDummyService on the IOHIDInterface (so neither IOHIDEventDriver nor the
// DriverKit AppleUserHIDEventDriver turns the pad into keyboard/pointer events) and an IOHIDProviderPropertyMerger
// that sets HIDServiceSupport on the IOHIDDevice (so the HID event system makes a user-space service for the device
// and loads JoyConHIDServicePlugin on it). They match Bluetooth Joy-Cons (bluetoothd's IOHIDUserDevice) as well.
// The iOS 27 build adds no Nintendo entries: its prelinked kext's own driver serves Joy-Cons (SwitchJoyConHID.cpp).
// Idempotent; thread context; panics with the reason on failure.
void switch_joycon_hid_prepare();

// The Home (R) or Capture (L) bit of a running rail Joy-Con's 0x21/0x30/0x31 report to the system-buttons device, made
// by the first call and kept for the boot; whether or not the Joy-Con's game controller device exists. Rail work loop.
void switch_joycon_hid_buttons(bool left, const uint8_t *report, size_t length);

// The Joy-Con's system button up, if it was down (its session ended). Rail work loop.
void switch_joycon_hid_buttons_release(bool left);

// Creates, attaches (under IOResources) and starts a Joy-Con device. `mac` is the Joy-Con's BD_ADDR in display order.
// Output reports go to `output(context, ...)`, and the end of its backboardd session to `ended(context, ...)`, until
// switch_joycon_hid_destroy(). Rail work loop; panics on failure.
IOService *switch_joycon_hid_create(bool left, const uint8_t mac[6], SwitchJoyConOutput output,
                                    SwitchJoyConSessionEnded ended, void *context);

// Hands one input report (report ID first) to the device; 0x21 and 0x30 are padded to the descriptor's 49 bytes,
// longer reports are cut to the descriptor's size (0x31-0x33: 362 bytes with the ID). Call from the rail driver's work
// loop.
IOReturn switch_joycon_hid_input(const IOService *device, const uint8_t *report, size_t length);

// Stops output and session callbacks (output returns kIOReturnOffline from here on; the closes the termination causes
// report nothing), terminates the device and drops the creator's reference. gamecontrollerd sees the controller
// disconnect. Rail work loop.
void switch_joycon_hid_destroy(const IOService *device);
