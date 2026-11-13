// SPDX-License-Identifier: BSD-2-Clause-Patent
// Joy-Con HID devices for the rail driver (see SwitchJoyConHID.h for what iOS does with them).
//
// SwitchJoyConDevice subclasses the original IOHIDDevice through NativeHidInput's qualified ABI and bindings
// (kits/SwitchABI: OriginalHidDeviceABI.h, hid-original-bindings.o), like ../SwitchTouch/SwitchHIDDevice.cpp,
// so it is a late object (Drivers.cmake switch_late_object) whose static initializer runs from
// switch_joycon_hid_prepare() once IOHIDFamily is loaded. Overrides: NativeHidInput's set (free, start, stop,
// getWorkLoop, newTransportString, newProductString, newReportDescriptor; the bindings leave exactly those IOHIDDevice
// slots unbound), slot 183, setReport (below), handleOpen/handleClose, which call the bound originals and keep the
// device's backboardd session (below), and on 27 the identity getters (below).
//
// Unlike the touch screen and buttons, no IOHIDEventDriver may sit on these devices. iPadOS 17's own Joy-Con support
// is user space (all verified in the 21H461 OS image):
//   * AppleGameControllerPersonality.kext Info.plist, "Nintendo JoyCon L/R IOService" and "... Property Merge" (and the
//     same pair for Pro Controller 0x2009, SNES 0x2017, N64 0x2019): IOHIDEventDummyService on the IOHIDInterface,
//     HIDServiceSupport merged into the IOHIDDevice. That kext is in the J120 kernelcache but not in the default
//     prelink set, so prepare() adds the same personalities (IOHIDFamily owns both classes); when the kext is
//     prelinked, its own entries are taken out (switch_joycon_hid_select_personalities).
//   * HIDServiceSupport makes backboardd's HID event system create a user-space service for the IOHIDDevice
//     (IOHIDEventServicePlugin.plugin matches HIDServiceSupport=true), and JoyConHIDServicePlugin.plugin (Matching
//     PrimaryUsagePage 1 / PrimaryUsage 5, which the descriptor below declares) claims it:
//     +[JoyConHIDServicePlugin matchService:options:score:] accepts VendorID 0x57e with ProductID 0x2006, 0x2007, 0x2009,
//     0x2017 or 0x2019 (read from the registry). Its base class reads Transport ("Bluetooth"/"USB"/
//     "BluetoothLowEnergy"), isVirtual and SerialNumber, opens the device, and only for Transport "Bluetooth" attaches a
//     BTSession, which it uses to disconnect an idle controller by SerialNumber (BTDeviceAddressFromString; a no-op
//     for a Joy-Con that is on the rail rather than connected). In GameController.framework,
//     isDeviceParentAuthenticated runs the MFi parent check only for Transport "USB", -[_GCDefaultPhysicalDevice
//     updateGlyphFlags] looks at External Accessory devices for "Bluetooth", and the controller identifier
//     (+[_GCDefaultPhysicalDevice identifierForService:], -[_GCNintendoJoyConDevice initWithHIDDevice:manager:type:])
//     is PhysicalDeviceUniqueID, else SerialNumber, else a random UUID. So "Bluetooth" with the Joy-Con's BD_ADDR as
//     SerialNumber is the transport that costs nothing and keeps one identity for a Joy-Con on and off the rail.
//   * The plugin speaks the Nintendo protocol itself: on activate it sends subcommands 0x08 (low power off), 0x02
//     (device info), 0x10 (SPI reads: colours, stick/IMU calibration), then 0x30 (player lights), 0x48 (vibration on),
//     and later 0x03/0x40 when gamecontrollerd asks for an input mode or motion. Each subcommand is one setReport of
//     report 0x01 (11 bytes + data: id, counter, 8 rumble bytes, subcommand), one at a time with a 2 s timeout, its
//     reply an input report 0x21 (>= 15 bytes, subcommand id at byte 14). Rumble is setReport 0x10 (10 bytes). Input
//     0x30 needs >= 49 bytes (0x31 with the ID), 0x3F is the simple report; other IDs are ignored.
//   * gamecontrollerd (launched by IOKit matching on IOHIDDevice with DeviceUsagePairs GD/Gamepad) gets the parsed
//     controller from the plugin and publishes GCController "Joy-Con (L)"/"Joy-Con (R)" (GCExtendedGamepad sideways,
//     motion, player lights, rumble). Holding Capture (L) and Home (R) together fuses the two into "Joy-Con (L/R)".
// So this device only has to look like bluetoothd's Joy-Con IOHIDUserDevice: same IDs, descriptor and report bytes.
//
// iPadOS 27 (24B119, its kext prelinked too) moves the claim off the IOHIDDevice. Its "Nintendo JoyCon L/R IOService"
// personalities put AppleGCHIDUserEventDriver (an IOHIDEventService, GameControllerCategory "switch", score 1000) on
// the IOHIDInterface. Its "GC User Driver Service" merger then sets GameControllerSupport and HIDServiceSupport on that
// driver. JoyConHIDServicePlugin matches IOPropertyMatch {GameControllerSupport, GameControllerCategory "switch"} and
// still speaks to the IOHIDDevice through HIDDevice (synchronous setReport = slot 183, as on 17). So the 27 build adds
// neither the dummy personalities, which would take the interface from Apple's driver, nor HIDServiceSupport on the
// device.
//
// The plugin's matchService now reads VendorID, ProductID (and PrimaryUsage) from that driver, not the device.
// IOHIDEventService::start sets each one from its IOHIDInterface (0 when missing), and IOHIDDevice::publishProperties
// gives the interface only what the device's new*() getters return; a property set on the device reaches personality
// matching (IOHIDInterface::matchPropertyTable asks the device) but not the interface. So on 27 the device answers
// newManufacturerString, newVendorIDNumber, newProductIDNumber and newSerialNumberString, as bluetoothd's
// IOHIDUserDevice does, and publishProperties puts them on the device and the interface.
//
// With one addition: a rail device outlives a backboardd restart (a respring), and gamecontrollerd takes only one
// plugin session per HID service, so the restarted backboardd's plugin and GamepadHIDServiceFilter would be refused
// and reconnect in a loop (gamecontrollerd the top CPU user). So handleOpen/handleClose keep the device's backboardd
// session (SwitchJoyConSession.h: the first backboardd user client to open it, in practice the plugin's HIDDevice) and
// report its end, the serving client's close or a different backboardd's open, once to the rail driver, which
// publishes a new device for the new backboardd (SwitchJoyConRail.cpp, "Sessions").
//
// The descriptor is the Joy-Con's own, from its SDP HIDDescriptorList (attribute 0x0206) as captured in switchroot's
// l4t-initramfs init (line 247): 170 bytes, Generic Desktop/Game Pad; inputs 0x21, 0x30 (48 bytes), 0x31-0x33
// (361), 0x3F (16 buttons, hat, 4 x 16-bit axes); outputs 0x01, 0x10, 0x11, 0x12 (48 bytes).
//
// System buttons. On the rails, Home (Joy-Con (R)) and Capture (Joy-Con (L)) are also the iPad's Home button and
// screenshot key, through SwitchJoyConButtons: one built-in Consumer Control device (an IOHIDEventDriver turns its
// reports into keyboard events, as for ../SwitchTouch/SwitchHIDDevice.cpp's console buttons) with two usages, each
// mirroring its Joy-Con button (byte 4 of reports 0x21/0x30/0x31: Home 0x10, Capture 0x20) press for press:
//   * Home -> Consumer Menu (0x0C/0x40), J120's own Home button: 21H461 AppleM68Buttons maps the ADT button-names
//     hold/menu/volup/voldown to 0x0C/0x30, 0x40, 0xE9, 0xEA. SpringBoard (-[SpringBoard __handleHIDEvent:withUIEvent:])
//     sends 0x0C/0x40 to -[SBHomeHardwareButton emulateHomeButtonEventsIfNeeded:], which for a sender without press
//     counts counts the presses itself ("SpringBoard is providing emulation"): one press goes home (and wakes the
//     screen), two open the app switcher, as the hardware button does.
//   * Capture -> Consumer Snapshot (0x0C/0x65): the same SpringBoard method hands it to
//     -[SpringBoard _handleSnapshotButtonHIDEvent:buttonIsDown:fromSource:], which on the release calls takeScreenshot
//     (flash, thumbnail, saved to Photos) unless the screen is off ("Snapshot button triggering screenshot").
// Not a Home + Lock chord: this SpringBoard runs with homeButtonType 2 (it logs "screenshot configured for lock +
// volume up"; -[SpringBoard _completeStartupAfterMainSceneConnect:] gives the Screenshot recognizer
// lock + home only for other types). Not GameController's own handling either: for a single Joy-Con
// JoyConHIDServicePlugin turns either button into one 0x0C/0x223 (AC Home) click at its release
// (-[JoyConHIDServicePlugin homeButtonPressed:], 3 s hold = the combine gesture), and SpringBoard's SB-Default rule
// (-[SBHIDEventDispatchController init]) sends 0x0C/0x223 from gamepads to the focused app, where it does nothing
// ("ACHome page:0xC usage:0x223 ... source:gamepad"). The Joy-Con reports still reach
// the plugin unchanged, so games see both buttons and the combine gesture works. Backboardd routes the device like
// the console buttons (Built-In, Consumer Control: "(display: builtin) desc: (Keyboard) -> <system>").
// Bluetooth Joy-Cons and Pro Controllers get the same two usages, clicked on the console buttons device from their
// reports on the way to bluetoothd (../SwitchSuspend/SwitchWakeSources.h, "Bluetooth controllers' Home and Capture"):
// 21H461's GameController gives Nintendo devices no game-intent component, so their Home reaches nothing system-wide.
#include "OriginalHidDeviceABI.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCatalogue.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOUserClient.h>
#include <sys/param.h>
#include <sys/proc.h>
#include "SwitchJoyConHID.h"
#include "SwitchJoyConSession.h"
#include "../SwitchSuspend/SwitchWake.h"

extern "C" OSReturn switch_kernel_classes(void (*initialize)());
extern "C" void switch_joycon_hid_static_init(void) __asm__("__GLOBAL__sub_I_SwitchJoyConHID.cpp");

namespace {
constexpr uint32_t kNintendo = 0x057e;
#if !XNU_12377_121_6
// Over IOHIDEventDriver's own scores and AppleUserHIDEventDriver's 1, as ../SwitchTouch/SwitchHIDDevice.cpp.
constexpr uint32_t kDummyScore = 10000, kMergerScore = 1000;
// AppleGameControllerPersonality's Nintendo entries: Joy-Con L, Joy-Con R, Pro Controller, SNES, N64.
constexpr uint32_t kNintendoProducts[] = {0x2006, 0x2007, 0x2009, 0x2017, 0x2019};
#endif
constexpr size_t kLargestInput = 1 + 0x169;  // reports 0x31-0x33
constexpr size_t kStandardInput = 1 + 0x30;  // reports 0x21 and 0x30
// AppleGameControllerPersonality.kext (21H461 Info.plist, 45 personalities): 44 IOHIDEventDummyService /
// IOHIDProviderPropertyMerger pairs for Xbox (0x045e), Sony (0x054c), Luna (0x0171) and Nintendo ProductIDs, whose
// classes are IOHIDFamily's, so OSKext marks them IOPersonalityPublisher = the kext; and "Generic Controller
// IOService", the kext's own AppleGCHIDEventDummyService (score 10000 on GD Joystick/Game Pad interfaces), which
// claims an interface only when gamecontrollerd has set GameControllerSupportedHIDDevice on the IOHIDDevice for a pad
// in its controller database, and then sets HIDServiceSupport/GamepadHIDServiceSupport for
// GenericGamepadHIDServicePlugin.
constexpr char kAppleGCBundle[] = "com.apple.driver.AppleGameControllerPersonality";

const uint8_t kJoyConDescriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x06, 0x01, 0xff, 0x85, 0x21, 0x09, 0x21, 0x75, 0x08, 0x95, 0x30, 0x81, 0x02,
    0x85, 0x30, 0x09, 0x30, 0x75, 0x08, 0x95, 0x30, 0x81, 0x02, 0x85, 0x31, 0x09, 0x31, 0x75, 0x08, 0x96, 0x69, 0x01,
    0x81, 0x02, 0x85, 0x32, 0x09, 0x32, 0x75, 0x08, 0x96, 0x69, 0x01, 0x81, 0x02, 0x85, 0x33, 0x09, 0x33, 0x75, 0x08,
    0x96, 0x69, 0x01, 0x81, 0x02, 0x85, 0x3f, 0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
    0x95, 0x10, 0x81, 0x02, 0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x05,
    0x09, 0x75, 0x04, 0x95, 0x01, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x34, 0x16, 0x00,
    0x00, 0x27, 0xff, 0xff, 0x00, 0x00, 0x75, 0x10, 0x95, 0x04, 0x81, 0x02, 0x06, 0x01, 0xff, 0x85, 0x01, 0x09, 0x01,
    0x75, 0x08, 0x95, 0x30, 0x91, 0x02, 0x85, 0x10, 0x09, 0x10, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02, 0x85, 0x11, 0x09,
    0x11, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02, 0x85, 0x12, 0x09, 0x12, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02, 0xc0,
};
static_assert(sizeof(kJoyConDescriptor) == 170, "Joy-Con SDP report descriptor");

// System buttons (file comment). Report ID 1, bit 0 Menu, bit 1 Snapshot.
constexpr uint32_t kButtonsProduct = 0x5104;   // after ../SwitchTouch's 0x5101-0x5103
constexpr uint32_t kEventDriverScore = 10000;  // as SwitchHIDDevice.cpp: over IOHIDEventDriver's own and the dext's 1
constexpr size_t kSharedButtons = 4;           // report byte: Minus, Plus, sticks, Home 0x10, Capture 0x20
constexpr uint8_t kHome = 0x10, kCapture = 0x20;

enum : uint8_t {
    kMenu = 1u << 0,
    kSnapshot = 1u << 1
};

const uint8_t kButtonsDescriptor[] = {
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x01,  // Consumer: Consumer Control, Collection (Application), Report ID 1
    0x09, 0x40, 0x09, 0x65,                          // Menu, Snapshot
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02, 0x95, 0x06, 0x81, 0x03, 0xc0,
};
}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage (its kalloc type view)
class SwitchJoyConDevice : public IOHIDDevice {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchJoyConDevice) IOWorkLoop *loop = nullptr;
    IOBufferMemoryDescriptor *input = nullptr;
    IOLock *outputLock = nullptr;  // output, ended, outputContext and session
    SwitchJoyConOutput output = nullptr;
    SwitchJoyConSessionEnded ended = nullptr;
    void *outputContext = nullptr;
    switch_joycon_session session = {};
    bool left = false;
    char serial[18] = {};

    // Reports the end of the backboardd session (SwitchJoyConHID.h) once, while the rail still listens.
    void noteSession(switch_joycon_session_event event, int32_t openerPid)
    {
        if (event == kSwitchJoyConSessionNone || !ended)
            return;
        ended(outputContext, session.pid, openerPid);
    }

public:
    bool initWith(bool isLeft, const uint8_t mac[6], SwitchJoyConOutput fn, SwitchJoyConSessionEnded endedFn,
                  void *context)
    {
        left = isLeft;
        output = fn;
        ended = endedFn;
        outputContext = context;
        snprintf(serial, sizeof(serial), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
                 mac[5]);
        outputLock = IOLockAlloc();
        return (outputLock != nullptr) && init();
    }

    IOWorkLoop *getWorkLoop() const override
    {
        return loop;
    }

    // What the plugin and gamecontrollerd see for a Bluetooth Joy-Con; see the file comment.
    OSString *newTransportString() const override
    {
        return OSString::withCString("Bluetooth");
    }

    OSString *newProductString() const override
    {
        return OSString::withCString(left ? "Joy-Con (L)" : "Joy-Con (R)");
    }

#if XNU_12377_121_6
    OSString *newManufacturerString() const override
    {
        return OSString::withCString("Nintendo");
    }

    OSNumber *newVendorIDNumber() const override
    {
        return OSNumber::withNumber(kNintendo, 32);
    }

    OSNumber *newProductIDNumber() const override
    {
        return OSNumber::withNumber(left ? kSwitchJoyConLeftPID : kSwitchJoyConRightPID, 32);
    }

    OSString *newSerialNumberString() const override
    {
        return OSString::withCString(serial);
    }
#endif

    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override
    {
        *out = IOBufferMemoryDescriptor::withBytes(kJoyConDescriptor, sizeof(kJoyConDescriptor), kIODirectionNone);
        return *out ? kIOReturnSuccess : kIOReturnNoMemory;
    }

    // Slot 183 is IOHIDDevice::setReport(IOMemoryDescriptor *, IOHIDReportType, IOOptionBits), the three-argument form:
    // IOHIDLibUserClient calls it for a synchronous setReport (with options = report ID, IOHIDLibUserClient.cpp:1983)
    // and IOHIDDevice's async-report thread for queued ones (IOHIDFamily-2031.120.4, the 21H461 kext's version:
    // IOHIDDevice.h:506, the virtual order 168 handleStart .. 181 handleReport, 182 getReport, 183 setReport). The
    // original at 0xfffffff004add294 traces kHIDTraceSetReport and tail-calls slot 194 (the five-argument setReport)
    // with (report, type, options, 0, 0), which confirms the slot. The generated ABI only knows it as opaque183, so this
    // class overrides that name; its definition (after the class) is one branch to switch_joycon_set_report with the
    // argument registers (this, report, type, options) untouched.
    void opaque183() override;

    IOReturn sendReport(IOMemoryDescriptor *report, uint32_t type, IOOptionBits options)
    {
        if (type != 1 /* kIOHIDReportTypeOutput */ || !report)
            return kIOReturnUnsupported;
        uint8_t bytes[64];
        const uint8_t id = static_cast<uint8_t>(options & 0xff);
        const size_t length = report->getLength();
        if (!length || length > sizeof(bytes) - 1)
            return kIOReturnBadArgument;
        // HIDDevice/IOHIDDeviceSetReport hand over the report with its ID byte first (the plugin builds it that way);
        // a caller that left it out still gets a report that starts with the ID.
        size_t at = 0;
        if (report->readBytes(0, bytes, 1) != 1)
            return kIOReturnError;
        if (id && bytes[0] != id)
            bytes[at++] = id;
        if (report->readBytes(0, bytes + at, length) != length)
            return kIOReturnError;
        IOLockLock(outputLock);
        const IOReturn result = output ? output(outputContext, bytes, at + length) : kIOReturnOffline;
        IOLockUnlock(outputLock);
        return result;
    }

    void detachOutput()
    {
        IOLockLock(outputLock);
        output = nullptr;
        ended = nullptr;
        outputContext = nullptr;
        IOLockUnlock(outputLock);
    }

    // IOService::open/close call these with the device's arbitration lock held, on the opener's thread for a user
    // client (IOHIDLibUserClient opens from its external method, through its gate: the same thread) and on a kernel
    // thread for a close when the client's task has gone. The originals are IOHIDDevice's (bound); the session needs
    // only the client's identity, compared and never dereferenced.
    bool handleOpen(IOService *client, IOOptionBits options, void *arg) override
    {
        if (!IOHIDDevice::handleOpen(client, options, arg))
            return false;
        if (!OSDynamicCast(IOUserClient, client))
            return true;
        char name[MAXCOMLEN + 1] = {};
        proc_selfname(name, sizeof(name));
        const int32_t pid = proc_selfpid();
        IOLockLock(outputLock);
        noteSession(switch_joycon_session_open(&session, reinterpret_cast<uintptr_t>(client), pid,
                                               strncmp(name, "backboardd", sizeof(name)) == 0),
                    pid);
        IOLockUnlock(outputLock);
        return true;
    }

    void handleClose(IOService *client, IOOptionBits options) override
    {
        IOHIDDevice::handleClose(client, options);
        IOLockLock(outputLock);
        noteSession(switch_joycon_session_close(&session, reinterpret_cast<uintptr_t>(client)), 0);
        IOLockUnlock(outputLock);
    }

    bool start(IOService *provider) override
    {
        loop = IOWorkLoop::workLoop();
        input = IOBufferMemoryDescriptor::withCapacity(kLargestInput, kIODirectionNone);
        if (!loop || !input)
            return false;
#if !XNU_12377_121_6
        // bluetoothd's HID devices carry these (com.apple.MobileBluetooth.InputDevice); HIDServiceSupport is what the
        // property-merger personality would add, set here as well so the device does not depend on its probe order.
        setProperty("VendorID", static_cast<unsigned long long>(kNintendo), 32);
        setProperty("ProductID", static_cast<unsigned long long>(left ? kSwitchJoyConLeftPID : kSwitchJoyConRightPID),
                    32);
        setProperty("Manufacturer", "Nintendo");
        setProperty("SerialNumber", serial);
        setProperty("HIDServiceSupport", true);
#endif
        return IOHIDDevice::start(provider);
    }

    // Overridden only because the bindings name every slot NativeHidInput does not override, and it overrides stop:
    // a slot this class inherited would be an unbound IOHIDDevice::stop import. The qualified call below goes through
    // the original vtable (-fapple-kext).
    void stop(IOService *provider) override
    {
        detachOutput();
        IOHIDDevice::stop(provider);
    }

    void free() override
    {
        OSSafeReleaseNULL(input);
        OSSafeReleaseNULL(loop);
        if (outputLock)
            IOLockFree(outputLock);
        outputLock = nullptr;
        IOHIDDevice::free();
    }

    IOReturn deliver(const uint8_t *report, size_t length)
    {
        if (!length)
            return kIOReturnBadArgument;
        if (length > kLargestInput)
            length = kLargestInput;
        // The rail sends the reports the Joy-Con would send over Bluetooth, but without the padding to the report size
        // the descriptor declares; the plugin drops a 0x30 shorter than 49 bytes.
        const size_t size =
            ((report[0] == 0x21 || report[0] == 0x30) && length < kStandardInput) ? kStandardInput : length;
        uint8_t *bytes = static_cast<uint8_t *>(input->getBytesNoCopy());
        memcpy(bytes, report, length);
        memset(bytes + length, 0, size - length);
        input->setLength(size);
        return handleReport(input, 0 /* kIOHIDReportTypeInput */, 0);
    }
};

OSDefineMetaClassAndStructors(SwitchJoyConDevice, IOHIDDevice)

    extern "C" __attribute__((used)) IOReturn
    switch_joycon_set_report(SwitchJoyConDevice *device, IOMemoryDescriptor *report, uint32_t type,
                             IOOptionBits options)
{
    return device->sendReport(report, type, options);
}

// SwitchJoyConDevice::opaque183(), see the declaration. Darwin assembler: one statement per line.
__asm__(".text\n"
        ".globl __ZN18SwitchJoyConDevice9opaque183Ev\n"
        ".p2align 2\n"
        "__ZN18SwitchJoyConDevice9opaque183Ev:\n"
        "  b _switch_joycon_set_report\n");

// The rail Joy-Cons' Home and Capture as Consumer Menu and Snapshot (file comment). Overrides exactly NativeHidInput's
// set, as SwitchHIDDevice does. Created with the first rail Joy-Con and kept for the boot; every call comes from the
// rail driver's work loop (input, attach and detach all run there), so the state needs no lock.
// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage (its kalloc type view)
class SwitchJoyConButtons : public IOHIDDevice {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchJoyConButtons) IOWorkLoop *loop = nullptr;
    IOBufferMemoryDescriptor *report = nullptr;
    uint8_t held[2] = {};  // per Joy-Con (0 R, 1 L): its button, kHome or kCapture, while pressed
    uint8_t posted = 0;    // kMenu | kSnapshot as last reported

public:
    IOWorkLoop *getWorkLoop() const override
    {
        return loop;
    }

    OSString *newTransportString() const override
    {
        return OSString::withCString("UART");
    }

    OSString *newProductString() const override
    {
        return OSString::withCString("Joy-Con System Buttons");
    }

    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override
    {
        *out = IOBufferMemoryDescriptor::withBytes(kButtonsDescriptor, sizeof(kButtonsDescriptor), kIODirectionNone);
        return *out ? kIOReturnSuccess : kIOReturnNoMemory;
    }

    bool start(IOService *provider) override
    {
        loop = IOWorkLoop::workLoop();
        report = IOBufferMemoryDescriptor::withCapacity(2, kIODirectionNone);
        if (!loop || !report)
            return false;
        // Matched by this file's IOHIDEventDriver personality; Built-In makes backboardd treat it as the console buttons.
        setProperty("VendorID", static_cast<unsigned long long>(kNintendo), 32);
        setProperty("ProductID", static_cast<unsigned long long>(kButtonsProduct), 32);
        setProperty("Manufacturer", "Nintendo");
        setProperty("Built-In", true);
        return IOHIDDevice::start(provider);
    }

    void stop(IOService *provider) override
    {
        IOHIDDevice::stop(provider);
    }

    void free() override
    {
        OSSafeReleaseNULL(report);
        OSSafeReleaseNULL(loop);
        IOHIDDevice::free();
    }

    // `pressed` is the Joy-Con's own system button (kHome on the right, kCapture on the left) or 0.
    void update(bool isLeft, uint8_t pressed)
    {
        held[isLeft ? 1 : 0] = pressed;
        const uint8_t any = held[0] | held[1];
        const uint8_t now = static_cast<uint8_t>(((any & kHome) ? kMenu : 0u) | ((any & kCapture) ? kSnapshot : 0u));
        if (now == posted)
            return;
        // Home down with the panel off: a wake source (journal, wake guarantee); the Menu press below does the waking.
        if ((now & kMenu) && !(posted & kMenu))
            switch_wake_event(SWITCH_WAKE_RAIL_HOME);
        posted = now;
        uint8_t *bytes = static_cast<uint8_t *>(report->getBytesNoCopy());
        bytes[0] = 1;
        bytes[1] = now;
        report->setLength(2);
        handleReport(report, 0 /* kIOHIDReportTypeInput */,
                     0);  // dropped by IOHIDFamily until the event driver opens it
    }
};

OSDefineMetaClassAndStructors(SwitchJoyConButtons, IOHIDDevice)

    namespace
{
    OSDictionary *dictionary(const char *const *keys, const OSObject *const *values, unsigned count)
    {
        OSDictionary *dict = OSDictionary::withCapacity(count);
        for (unsigned i = 0; dict && i < count; ++i) {
            if (!values[i] || !dict->setObject(keys[i], values[i]))
                OSSafeReleaseNULL(dict);
        }
        return dict;
    }

#if !XNU_12377_121_6
    // The two AppleGameControllerPersonality entries for one Nintendo ProductID, appended to `out`.
    bool addPersonalities(OSArray * out, uint32_t productID)
    {
        OSNumber *vendor = OSNumber::withNumber(kNintendo, 32);
        OSNumber *product = OSNumber::withNumber(productID, 32);
        OSNumber *dummyScore = OSNumber::withNumber(kDummyScore, 32);
        OSNumber *mergerScore = OSNumber::withNumber(kMergerScore, 32);
        OSString *bundle = OSString::withCString("com.apple.iokit.IOHIDFamily");
        OSString *dummyClass = OSString::withCString("IOHIDEventDummyService");
        OSString *interface = OSString::withCString("IOHIDInterface");
        OSString *mergerClass = OSString::withCString("IOHIDProviderPropertyMerger");
        OSString *device = OSString::withCString("IOHIDDevice");
        const OSBoolean *no = kOSBooleanFalse;
        const OSBoolean *yes = kOSBooleanTrue;

        const char *dummyKeys[] = {"CFBundleIdentifier", "IOClass",   "IOProviderClass",  "IOProbeScore",
                                   "VendorID",           "ProductID", "HIDServiceSupport"};
        const OSObject *dummyValues[] = {bundle, dummyClass, interface, dummyScore, vendor, product, no};
        OSDictionary *dummy = dictionary(dummyKeys, dummyValues, 7);

        const char *matchKeys[] = {"VendorID", "ProductID"};
        const OSObject *matchValues[] = {vendor, product};
        OSDictionary *match = dictionary(matchKeys, matchValues, 2);
        const char *mergeKeys[] = {"HIDServiceSupport"};
        const OSObject *mergeValues[] = {yes};
        OSDictionary *merge = dictionary(mergeKeys, mergeValues, 1);
        const char *mergerKeys[] = {"CFBundleIdentifier", "IOClass",         "IOProviderClass",
                                    "IOProbeScore",       "IOPropertyMatch", "IOProviderMergeProperties"};
        const OSObject *mergerValues[] = {bundle, mergerClass, device, mergerScore, match, merge};
        OSDictionary *merger = dictionary(mergerKeys, mergerValues, 6);

        const bool ok = (dummy != nullptr) && (merger != nullptr) && out->setObject(dummy) && out->setObject(merger);
        OSObject *all[] = {vendor,      product, dummyScore, mergerScore, bundle, dummyClass, interface,
                           mergerClass, device,  dummy,      match,       merge,  merger};
        for (const OSObject *object : all) {
            if (object)
                object->release();
        }
        return ok;
    }
#endif
}  // namespace

// IOHIDEventDriver for SwitchJoyConButtons only, as SwitchHIDDevice.cpp's personality(): IOHIDFamily's
// MatchPropertyTable compares VendorID, ProductID and the DeviceUsagePairs against the device.
static void addButtonsPersonality()
{
    OSNumber *page = OSNumber::withNumber(0x0c, 32);
    OSNumber *usage = OSNumber::withNumber(0x01, 32);
    OSNumber *vendor = OSNumber::withNumber(kNintendo, 32);
    OSNumber *product = OSNumber::withNumber(kButtonsProduct, 32);
    OSNumber *score = OSNumber::withNumber(kEventDriverScore, 32);
    OSString *bundle = OSString::withCString("com.apple.iokit.IOHIDFamily");
    OSString *driverClass = OSString::withCString("IOHIDEventDriver");
    OSString *interface = OSString::withCString("IOHIDInterface");

    const char *pairKeys[] = {"DeviceUsagePage", "DeviceUsage"};
    const OSObject *pairValues[] = {page, usage};
    OSDictionary *pair = dictionary(pairKeys, pairValues, 2);
    const OSObject *pairObject = pair;
    OSArray *pairs = pair ? OSArray::withObjects(&pairObject, 1) : nullptr;
    const char *keys[] = {"CFBundleIdentifier", "IOClass",   "IOProviderClass", "IOProbeScore",
                          "VendorID",           "ProductID", "DeviceUsagePairs"};
    const OSObject *values[] = {bundle, driverClass, interface, score, vendor, product, pairs};
    OSDictionary *driver = dictionary(keys, values, 7);
    const OSObject *driverObject = driver;
    OSArray *personalities = driver ? OSArray::withObjects(&driverObject, 1) : nullptr;
    if (!personalities)
        panic("SwitchJoyCon: no memory for the system-buttons personality");
    if (!gIOCatalogue->addDrivers(personalities, true)) {
        panic("SwitchJoyCon: IOCatalogue::addDrivers refused the system-buttons personality");
    }
    OSObject *all[] = {page,        usage,     vendor, product, score,  bundle,
                       driverClass, interface, pair,   pairs,   driver, personalities};
    for (const OSObject *object : all) {
        if (object)
            object->release();
    }
}

#if !XNU_12377_121_6
static void addNintendoPersonalities()
{
    OSArray *personalities = OSArray::withCapacity(2 * sizeof(kNintendoProducts) / sizeof(kNintendoProducts[0]));
    for (const uint32_t product : kNintendoProducts) {
        if (!personalities || !addPersonalities(personalities, product))
            panic("SwitchJoyCon: no memory for the personalities");
    }
    if (!gIOCatalogue->addDrivers(personalities, true))
        panic("SwitchJoyCon: IOCatalogue::addDrivers refused the personalities");
    personalities->release();
}
#endif

// A one-key matching dictionary for IOCatalogue (which compares only the keys it carries).
static OSDictionary *catalogueMatch(const char *key, const char *value)
{
    const OSString *string = OSString::withCString(value);
    OSDictionary *dict = string ? OSDictionary::withCapacity(1) : nullptr;
    if (dict && !dict->setObject(key, string))
        OSSafeReleaseNULL(dict);
    OSSafeReleaseNULL(string);
    if (!dict)
        panic("SwitchJoyCon: no memory for a catalogue lookup");
    return dict;
}

// The prelinked kext's personalities reach the catalogue at bootstrap (OSKext::sendAllKextPersonalitiesToCatalog);
// none can match before a game controller exists, and none exists before the board thread (Bluetooth HID devices
// come from bluetoothd, USB is not integrated), so taking them out here is safe. Both removal keys are needed:
// IOPersonalityPublisher marks the 44 IOHIDFamily-class entries, CFBundleIdentifier the kext's own class. Double
// matching cannot happen with both sets present either (same classes, one default match category: one
// IOHIDEventDummyService wins per interface; mergers fail their probe after setting absent keys), but one source of
// truth is simpler to reason about.
void switch_joycon_hid_select_personalities()
{
    static bool done;
    if (done)
        return;
    done = true;
    OSDictionary *byClass = catalogueMatch("IOClass", "AppleGCHIDEventDummyService");
    SInt32 generation = 0;
    const OSOrderedSet *found = gIOCatalogue->findDrivers(byClass, &generation);
    const bool present = (found != nullptr) && (found->getCount() != 0u);
    OSSafeReleaseNULL(found);
    OSSafeReleaseNULL(byClass);
    if (!present)
        return;
    static const char *const keys[] = {kIOPersonalityPublisherKey, kCFBundleIdentifierKey};
    for (const char *key : keys) {
        OSDictionary *match = catalogueMatch(key, kAppleGCBundle);
        gIOCatalogue->removeDrivers(match, false);
        match->release();
    }
}

void switch_joycon_hid_prepare()
{
    static IOLock *volatile lock;
    static bool ready;
    if (!lock) {
        IOLock *fresh = IOLockAlloc();
        if (fresh && !OSCompareAndSwapPtr(nullptr, fresh, &lock))
            IOLockFree(fresh);
    }
    if (!lock)
        panic("SwitchJoyCon: no memory for the HID lock");
    IOLockLock(lock);
    if (!ready) {
        const OSReturn loaded = OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
        if (loaded != kOSReturnSuccess)
            panic("SwitchJoyCon: loading com.apple.iokit.IOHIDFamily failed: %#x", loaded);
        const OSReturn registered = switch_kernel_classes(switch_joycon_hid_static_init);
        if (registered != kOSReturnSuccess)
            panic("SwitchJoyCon: registering SwitchJoyConDevice failed: %#x", registered);

#if !XNU_12377_121_6
        addNintendoPersonalities();
#endif
        addButtonsPersonality();
        ready = true;
    }
    IOLockUnlock(lock);
}

static SwitchJoyConButtons *gButtons;  // rail work loop only (see the class)

static SwitchJoyConButtons *systemButtons()
{
    if (gButtons)
        return gButtons;
    switch_joycon_hid_prepare();
    IOService *resources = IOService::getResourceService();
    auto *buttons = OSTypeAlloc(SwitchJoyConButtons);
    if (!resources || !buttons || !buttons->init() || !buttons->attach(resources))
        panic("SwitchJoyCon: system-buttons HID device: attach failed");
    if (!buttons->start(resources))
        panic("SwitchJoyCon: system-buttons HID device: IOHIDDevice::start failed");
    gButtons = buttons;  // kept for the boot
    return gButtons;
}

void switch_joycon_hid_buttons(bool left, const uint8_t *report, size_t length)
{
    // Reports 0x21, 0x30 and 0x31 start with the standard input (timer, battery, three button bytes); 0x3F does not.
    if (length > kSharedButtons && (report[0] == 0x21 || report[0] == 0x30 || report[0] == 0x31))
        systemButtons()->update(left, report[kSharedButtons] & (left ? kCapture : kHome));
}

void switch_joycon_hid_buttons_release(bool left)
{
    if (gButtons)
        gButtons->update(left, 0);
}

IOService *switch_joycon_hid_create(bool left, const uint8_t mac[6], SwitchJoyConOutput output,
                                    SwitchJoyConSessionEnded ended, void *context)
{
    switch_joycon_hid_prepare();
    IOService *resources = IOService::getResourceService();
    auto *device = OSTypeAlloc(SwitchJoyConDevice);
    if (!resources || !device || !device->initWith(left, mac, output, ended, context) || !device->attach(resources)) {
        panic("SwitchJoyCon: Joy-Con (%c) HID device: attach failed", left ? 'L' : 'R');
    }
    if (!device->start(resources))
        panic("SwitchJoyCon: Joy-Con (%c) HID device: IOHIDDevice::start failed", left ? 'L' : 'R');
    return device;  // the creator's reference, dropped by switch_joycon_hid_destroy
}

IOReturn switch_joycon_hid_input(const IOService *device, const uint8_t *report, size_t length)
{
    auto *joycon = OSDynamicCast(SwitchJoyConDevice, device);
    if (!joycon)
        return kIOReturnBadArgument;
    return joycon->deliver(report, length);
}

void switch_joycon_hid_destroy(const IOService *device)
{
    auto *joycon = OSDynamicCast(SwitchJoyConDevice, device);
    if (!joycon)
        return;
    joycon->detachOutput();
    joycon->terminate();
    // NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): drops switch_joycon_hid_create's reference (see the header)
    joycon->release();
}
