// SPDX-License-Identifier: BSD-2-Clause-Patent
// HID devices for the Switch's touch screen, buttons, ambient light sensor and motion sensors (SwitchHIDDevice.cpp), fed
// by SwitchTouch.cpp, ../SwitchButtons/SwitchButtons.cpp, ../SwitchBrightness/SwitchALS.cpp and
// ../SwitchMotion/SwitchMotion.cpp.
//
// Each device is an IOHIDDevice (the original class, through NativeHidInput's bindings) with a fixed report
// descriptor. Apple's in-kernel IOHIDEventDriver turns its reports into IOHIDEvents for backboardd, exactly as for a
// real HID transport. The DriverKit AppleUserHIDEventDriver (AppleUserHIDDrivers.dext) also matches IOHIDInterface
// for these usage pages, at IOProbeScore 1; it never starts here because this module adds its own IOHIDEventDriver
// personality per device (VendorID/ProductID match, IOProbeScore 10000, no IOMatchDefer), and IOService's
// probeCandidates skips a dext once the match category is consumed (canTerminateForReplacement needs
// IOPrimaryDriverTerminateOptions, which IOHIDEventDriver lacks).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <IOKit/IOReturn.h>
#include "../NativeMobileDisplay/SwitchPortrait.h"

class IOService;
class OSObject;

enum SwitchHIDKind : unsigned {
    kSwitchHIDTouch = 0,
    kSwitchHIDButtons = 1,
    kSwitchHIDLight = 2,
    kSwitchHIDAccelerometer = 3,
    kSwitchHIDGyro = 4,
    kSwitchHIDOrientation = 5,
    kSwitchHIDTemperature = 6,  // switch_hid_create_temperature only
};

// Touch screen, report ID 1: ten finger slots, slot n always carries contact n. Coordinates are panel pixels in
// the landscape frame iOS draws in (0..1279 left to right, 0..719 top to bottom); the descriptor's logical range
// is the whole panel, so backboardd scales them to whatever logical size the UI uses.
constexpr unsigned kSwitchTouchSlots = 10, kSwitchTouchWidth = 1280, kSwitchTouchHeight = 720;
// Portrait-native (SwitchPortrait.h): reports carry the panel's own frame instead, the one iOS is given as its screen
// (0..719 left to right, 0..1279 top to bottom of the portrait panel): landscape (x, y) is reported as (y, 1279 - x),
// the display's own mapping. backboardd's digitizer-to-panel rotation is 0, and iOS turns touches into its landscape
// interface itself.
constexpr unsigned kSwitchTouchReportWidth = SWITCH_PORTRAIT_NATIVE ? kSwitchTouchHeight : kSwitchTouchWidth,
                   kSwitchTouchReportHeight = SWITCH_PORTRAIT_NATIVE ? kSwitchTouchWidth : kSwitchTouchHeight;

enum : unsigned {
    kSwitchTouchTip = 1u << 0,
    kSwitchTouchValid = 1u << 1
};  // bit 2 (Untouch) stays 0

struct __attribute__((packed)) SwitchTouchSlot {
    uint8_t flags, contact;
    uint16_t x, y;
};

struct __attribute__((packed)) SwitchTouchReport {
    uint8_t id;  // 1
    SwitchTouchSlot slot[kSwitchTouchSlots];
};

// Buttons, report ID 1: one bit per Consumer usage (Power 0x30, Volume Increment 0xE9, Volume Decrement 0xEA, Menu 0x40:
// J120's Home button, Snapshot 0x65: the screenshot key; the last two clicked for a Bluetooth Nintendo controller's
// Home and Capture, ../SwitchSuspend/SwitchWake.h).
enum : uint8_t {
    kSwitchButtonPower = 1u << 0,
    kSwitchButtonVolumeUp = 1u << 1,
    kSwitchButtonVolumeDown = 1u << 2,
    kSwitchButtonHome = 1u << 3,
    kSwitchButtonSnapshot = 1u << 4,
};

struct __attribute__((packed)) SwitchButtonsReport {
    uint8_t id;  // 1
    uint8_t buttons;
};

// Ambient light sensor (../SwitchBrightness/SwitchALS.cpp): top-level collection Apple vendor 0xff00 / AmbientLightSensor
// 0x0004, CoreBrightness's ALS usage pair, with one Sensor-page Report Interval feature (IOHIDEventDriver starts only when
// it recognizes an element, and parseSensorPropertyElement takes that one). It posts no input reports: IOHIDEventDriver
// has no ambient light element, so SwitchALS makes the kIOHIDEventTypeAmbientLightSensor events itself and hands them to
// switch_hid_dispatch_event.

// Motion sensors (../SwitchMotion/SwitchMotion.cpp). Each is a top-level collection with the usage pair iPadOS 17's
// CoreMotion (CLIoHidInterface) and backboardd match, as the J120 device tree's AOP accel and gyro nodes declare
// (device-usage-page 0xff00, device-usage 3 and 9):
//  * Accelerometer, Apple vendor 0xff00/0x0003: input report 1 = Sensor-page Acceleration X/Y/Z (0x0453-0x0455) in
//    micro-g (unit exponent -6, so IOHIDEventDriver's kIOHIDEventTypeAccelerometer events carry g), feature report 1 =
//    Sensor Report Interval (0x030e) then Report Latency (0x031b), both microseconds: IOHIDEventDriver writes a
//    client's ReportInterval into the first and its BatchInterval (kIOHIDBatchIntervalKey) into the second, and
//    publishes ReportLatency, so the event system hands the batch interval to the device (IOHIDFamily-2031
//    IOHIDEventDriver.cpp parseSensorPropertyElement, setProperties).
//  * Gyroscope, Apple vendor 0xff00/0x0009: the same with Angular Velocity X/Y/Z (0x0457-0x0459) in micro-radians per
//    second (kIOHIDEventTypeGyro in rad/s).
//  * Device orientation, Sensor 0x20/0x008a (what CoreMotion's orientation notifier matches): input report 1 =
//    kSwitchOrientationDescriptor, SwitchOrientationReport.
// Vectors are in iPadOS's device frame: the portrait panel's (SwitchPortrait.h), +X to its right, +Y to its top, +Z
// out of the screen; acceleration is the gravity vector (lying face up reads z = -1 g), as Apple's AOP reports it.
struct __attribute__((packed)) SwitchMotionReport {
    uint8_t id;  // 1
    int32_t x, y, z;
};

// What 21H461 needs of an orientation report (IOHIDFamily's IOHIDEventDriver, CoreMotion's CLOrientationNotifier, in
// backboardd, SpringBoard and locationd alike):
//  * The event: a bit per AppleVendorMotion (0xff0c) usage 0x64 + UIDeviceOrientation (0x64 unknown .. 0x6a face down).
//    handleOrientationReport makes a kIOHIDEventTypeOrientation event (CMUsage) only for a bit that reads 1 and read 0
//    in the report before. The bits come twice: a report sets the orientation's bit in one set and clears the other,
//    and the next report uses the other set, so every report makes exactly one event, an unchanged orientation too.
//  * Its sample time: CoreMotion takes it only from the event's first child, which must be a vendor-defined event of
//    page 0xff15, usage 2 and 8 data bytes (a u64), and drops an event whose time is not above the last one's ("Skipping
//    stale orientation (N @ T)"); with no child the time is 0 and every event is dropped. The time is only compared
//    with earlier ones; it is mach absolute time here, the clock of the event's own timestamp, which CoreMotion hands
//    its clients.
//  * IOHIDEventDriver makes that child itself: each element inside an AppleVendor (0xff00) Message (0x23) collection
//    becomes, for every report of its ID, a vendor-defined event of the element's page and usage, version 0 and the
//    element's bytes, appended as a child to the next event it dispatches for that report; a report that makes no event
//    has its child dispatched alone, which CoreMotion logs as a wrong event type. Hence no report without an event.
//    The collection is Physical: an Application collection would be one of the device's usage pairs.
constexpr uint8_t kSwitchOrientationDescriptor[] = {
    0x05, 0x20, 0x09, 0x8a, 0xa1, 0x01, 0x85, 0x01,              // Sensor: Device Orientation, Collection (App), ID 1
    0x06, 0x0c, 0xff, 0x19, 0x64, 0x29, 0x6a,                    // AppleVendorMotion: Usage 0x64 .. 0x6a
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x07, 0x81, 0x02,  // 7 x 1 bit, Input (Data, Var, Abs)
    0x95, 0x01, 0x81, 0x03,                                      // 1 bit padding
    0x19, 0x64, 0x29, 0x6a, 0x95, 0x07, 0x81, 0x02,              // the second set
    0x95, 0x01, 0x81, 0x03,                                      // 1 bit padding
    0x06, 0x00, 0xff, 0x09, 0x23, 0xa1, 0x00,                    // AppleVendor: Message, Collection (Physical)
    0x06, 0x15, 0xff, 0x09, 0x02,                                // 0xff15 / 2: the sample time
    0x27, 0xff, 0xff, 0xff, 0x7f, 0x75, 0x40, 0x95, 0x01, 0x81, 0x02,  // 64 bits, Input (Data, Var, Abs)
    0xc0, 0xc0,
};

struct __attribute__((packed)) SwitchOrientationReport {
    uint8_t id;              // 1
    uint8_t orientation[2];  // 1u << UIDeviceOrientation in one set, 0 in the other
    uint64_t timestamp;      // strictly increasing from report to report
};

// Temperature sensors (../SwitchPower/SwitchSensors.cpp): thermalmonitord's HID sensors, Apple vendor 0xff00 /
// TemperatureSensor 0x0005 (the PrimaryUsagePage/PrimaryUsage it matches), one device per sensor, each with its own
// Product (thermalmonitord's matchName) and LocationID (the 4CC as a big-endian integer). No input reports: thermalmonitord
// reads them every 5 s with IOHIDServiceClientCopyEvent(kIOHIDEventTypeTemperature), which reaches the event driver's
// copyEvent through IOHIDEventServiceUserClient. IOHIDEventDriver::copyEvent answers keyboard, orientation and vendor
// events only, so each temperature device's driver gets a shared vtable copy whose copyEvent (slot 201) hands
// temperature requests to the device's handler (other types go to the original). Installed, after slots 199/200
// (getPrimaryUsagePage/getPrimaryUsage) have answered 0xff00/0x0005, from IOKit's first-publish notification for the
// driver, or by switch_hid_temperature_hook once the device's matching is quiet (the notification has not been seen
// to fire for these drivers on hardware).
// The handler runs on the client's thread (thermalmonitord's call through backboardd's event system), possibly inside
// the event service's command gate (the 38 devices share one work loop): it must not sleep long; it returns an event
// from switch_hid_temperature_event (+1) or nullptr (the read fails).
typedef OSObject *(*SwitchHIDCopyEventHandler)(void *context);
// Thread context; nullptr (one log line) on failure. Devices are created from one thread.
IOService *switch_hid_create_temperature(const char *product, uint32_t locationID, SwitchHIDCopyEventHandler handler,
                                         void *context);
// Whether that device's event driver has been found and hooked (then thermalmonitord's reads reach the handler).
bool switch_hid_temperature_hooked(IOService *device);

// Hooks the IOHIDEventDriver that has opened the device's interface, unless the publish notification has. Thread
// context, never waits.
enum SwitchHIDHook : unsigned {
    kSwitchHIDHooked = 0,       // by switch_hid_temperature_hook
    kSwitchHIDHookedAtPublish,  // by the publish notification
    kSwitchHIDHookNoDriver,     // no IOHIDEventDriver has opened its interface (yet)
    kSwitchHIDHookWrongSlots,   // slots 199/200 did not answer 0xff00/0x0005 (one log line, the first time)
    kSwitchHIDHookOtherVtable,  // its driver's vtable is not the one the other drivers share (one log line)
    kSwitchHIDHookNoMemory,
    kSwitchHIDHookKinds,
};

SwitchHIDHook switch_hid_temperature_hook(IOService *device);
// A kIOHIDEventTypeTemperature event (+1) with kIOHIDEventFieldTemperatureLevel = `fixed` (16.16 degrees C) at
// `timestamp` (mach absolute time); nullptr without memory, or for the rest of the boot (one log line) once the first
// event did not read back what was set.
OSObject *switch_hid_temperature_event(uint64_t timestamp, int32_t fixed);

// Creates, attaches (under IOResources) and starts the device. The first call loads IOHIDFamily, registers the
// device class and adds the IOHIDEventDriver personalities. Thread context; panics with the reason on failure.
IOService *switch_hid_create(SwitchHIDKind kind);

// Hands one input report to the device (and from there synchronously to its IOHIDEventDriver). Reports sent before
// the event driver has opened the device are dropped by IOHIDFamily. Call from one thread per device.
IOReturn switch_hid_post(IOService *device, const void *report, size_t length);

// The same with the report's own time (mach absolute time) instead of now, for samples a device delivers in batches
// (SwitchMotion) or after a bus read (SwitchTouch: the touch interrupt's time, r17-touch):
// IOHIDDevice::handleReportWithTime, which handleReport calls with the current time. Times must increase from report to
// report (IOHIDEventDriver takes an element whose time equals the report's as part of it). The touch device's edge
// wrapper is installed on this path too. kIOReturnUnsupported where switch_hid_supports_timestamps() is false.
IOReturn switch_hid_post_at(IOService *device, const void *report, size_t length, uint64_t timestamp);
bool switch_hid_supports_timestamps(IOService *device);

// Hands an IOHIDEvent to the device's IOHIDEventDriver as if the driver had made it (IOHIDEventService::dispatchEvent,
// the driver's slot 198). kIOReturnNotReady until the driver has opened the device. Call from one thread per device.
IOReturn switch_hid_dispatch_event(IOService *device, OSObject *event);

// Feature reports the event system sends the device: IOHIDEventDriver::setProperties writes a client's ReportInterval
// (microseconds; 0 when the last client stops) into the Report Interval element, and IOHIDDevice::postElementValues
// hands that feature report to the device's setReport (slot 183). Called with the report's bytes (the report ID first
// when the report carries one) on the thread that set the property, which may hold IOHIDEventDriver's work loop gate:
// it must not wait for anything. Devices without a handler answer kIOReturnUnsupported, as the original setReport does.
typedef void (*SwitchHIDFeatureHandler)(void *context, const uint8_t *report, size_t length);
void switch_hid_set_feature_handler(IOService *device, SwitchHIDFeatureHandler handler, void *context);

// Touch only: bit n set = contact n began within the edge band. Its finger events then carry
// kIOHIDDigitizerEventFromEdgeTip, which UIKit needs for the home and app-switcher edge gestures.
void switch_hid_set_edge_contacts(IOService *device, uint32_t contacts);
