// SPDX-License-Identifier: BSD-2-Clause-Patent
// Joy-Con HID devices for the rail driver (see SwitchJoyConHID.h for what iOS does with them).
//
// SwitchJoyConDevice subclasses the original IOHIDDevice through NativeHidInput's qualified ABI and bindings
// (kits/SwitchABI: OriginalHidDeviceABI.h, hid-original-bindings.o), so it is a late object whose static initializer
// runs from switch_joycon_hid_prepare() once IOHIDFamily is loaded. Each device shares its rail side's IOWorkLoop so
// input reports deliver directly via handleReportWithTime under the already-held workloop gate with zero cross-loop
// hops and no outputLock.
#include "OriginalHidDeviceABI.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCatalogue.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
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
#if SWITCH_RECORDER
// XNU kernel/switch/recorder (--//kernel:recorder): the owner saw a hitch; freeze the ring into a dump.
extern "C" __attribute__((weak)) void switch_recorder_mark(void)
{}
#endif

namespace {
constexpr uint32_t kNintendo = 0x057e;
#if !XNU_12377_121_6
// Over IOHIDEventDriver's own scores and AppleUserHIDEventDriver's 1, as ../SwitchTouch/SwitchHIDDevice.cpp.
constexpr uint32_t kDummyScore = 10000, kMergerScore = 1000;
// AppleGameControllerPersonality's Nintendo entries: Joy-Con L, Joy-Con R, Pro Controller, SNES, N64.
constexpr uint32_t kNintendoProducts[] = {0x2006, 0x2007, 0x2009, 0x2017, 0x2019};
constexpr char kAppleGCBundle[] = "com.apple.driver.AppleGameControllerPersonality";
#endif
constexpr size_t kLargestInput = 1 + 0x169;  // reports 0x31-0x33
constexpr size_t kStandardInput = 1 + 0x30;  // reports 0x21 and 0x30

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

constexpr uint32_t kButtonsProduct = 0x5104;   // after ../SwitchTouch's 0x5101-0x5103
constexpr uint32_t kEventDriverScore = 10000;  // over IOHIDEventDriver's own and the dext's 1

enum : uint8_t {
    kMenu = 1u << 0,
    kSnapshot = 1u << 1
};

const uint8_t kButtonsDescriptor[] = {
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x01,  // Consumer: Consumer Control, Collection (Application), Report ID 1
    0x09, 0x40, 0x09, 0x65,                          // Menu, Snapshot
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02, 0x95, 0x06, 0x81, 0x03, 0xc0,
};

struct ButtonsEdgeArgs {
    uint64_t timestamp;
    bool isLeft;
    bool down;
};
}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage (its kalloc type view)
class SwitchJoyConDevice : public IOHIDDevice {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchJoyConDevice) IOWorkLoop *loop = nullptr;
    IOBufferMemoryDescriptor *input = nullptr;
    // Immutable after initWith; `active` transitions 1 -> 0 once on detachOutput so callbacks stop without a lock.
    SwitchJoyConOutput output = nullptr;
    SwitchJoyConSessionEnded ended = nullptr;
    void *outputContext = nullptr;
    uint32_t generation = 0;
    uint32_t active = 0;
    uint64_t lastReportTime = 0;
    // Serialized by IOKit's device arbitration lock inside handleOpen/handleClose.
    switch_joycon_session session = {};
    bool left = false;
    char serial[18] = {};

    void noteSession(switch_joycon_session_event event, int32_t openerPid)
    {
        if (event == kSwitchJoyConSessionNone || !ended || !__atomic_load_n(&active, __ATOMIC_ACQUIRE))
            return;
        ended(outputContext, generation, session.pid, openerPid);
    }

public:
    bool initWith(IOWorkLoop *sharedLoop, bool isLeft, const uint8_t mac[6], uint32_t gen, SwitchJoyConOutput fn,
                  SwitchJoyConSessionEnded endedFn, void *context)
    {
        if (!sharedLoop)
            return false;
        left = isLeft;
        generation = gen;
        output = fn;
        ended = endedFn;
        outputContext = context;
        __atomic_store_n(&active, 1u, __ATOMIC_RELAXED);
        snprintf(serial, sizeof(serial), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
                 mac[5]);
        if (!init())
            return false;
        loop = sharedLoop;
        loop->retain();
        return true;
    }

    IOWorkLoop *getWorkLoop() const override
    {
        return loop;
    }

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

    // Slot 183 is IOHIDDevice::setReport(IOMemoryDescriptor *, IOHIDReportType, IOOptionBits).
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
        size_t at = 0;
        if (report->readBytes(0, bytes, 1) != 1)
            return kIOReturnError;
        if (id && bytes[0] != id)
            bytes[at++] = id;
        if (report->readBytes(0, bytes + at, length) != length)
            return kIOReturnError;
        if (!__atomic_load_n(&active, __ATOMIC_ACQUIRE) || !output)
            return kIOReturnOffline;
        return output(outputContext, generation, bytes, at + length);
    }

    void detachOutput()
    {
        __atomic_store_n(&active, 0u, __ATOMIC_RELEASE);
    }

    // IOService::open/close hold the device's arbitration lock around handleOpen/handleClose.
    bool handleOpen(IOService *client, IOOptionBits options, void *arg) override
    {
        if (!IOHIDDevice::handleOpen(client, options, arg))
            return false;
        if (!OSDynamicCast(IOUserClient, client))
            return true;
        char name[MAXCOMLEN + 1] = {};
        proc_selfname(name, sizeof(name));
        const int32_t pid = proc_selfpid();
        noteSession(switch_joycon_session_open(&session, reinterpret_cast<uintptr_t>(client), pid,
                                               strncmp(name, "backboardd", sizeof(name)) == 0),
                    pid);
        return true;
    }

    void handleClose(IOService *client, IOOptionBits options) override
    {
        IOHIDDevice::handleClose(client, options);
        noteSession(switch_joycon_session_close(&session, reinterpret_cast<uintptr_t>(client)), 0);
    }

    bool start(IOService *provider) override
    {
        input = IOBufferMemoryDescriptor::withCapacity(kLargestInput, kIODirectionNone);
        if (!loop || !input)
            return false;
#if !XNU_12377_121_6
        setProperty("VendorID", static_cast<unsigned long long>(kNintendo), 32);
        setProperty("ProductID", static_cast<unsigned long long>(left ? kSwitchJoyConLeftPID : kSwitchJoyConRightPID),
                    32);
        setProperty("Manufacturer", "Nintendo");
        setProperty("SerialNumber", serial);
        setProperty("HIDServiceSupport", true);
#endif
        return IOHIDDevice::start(provider);
    }

    void stop(IOService *provider) override
    {
        detachOutput();
        IOHIDDevice::stop(provider);
    }

    void free() override
    {
        OSSafeReleaseNULL(input);
        OSSafeReleaseNULL(loop);
        IOHIDDevice::free();
    }

    // Caller runs on `loop` (the shared rail workloop, gate already held).
    IOReturn deliver(const uint8_t *report, size_t length, uint64_t timestamp)
    {
        if (!length)
            return kIOReturnBadArgument;
        if (length > kLargestInput)
            length = kLargestInput;
        // Pad 0x21/0x30 reports to the descriptor's 49 bytes; the plugin drops shorter 0x30 reports.
        const size_t size =
            ((report[0] == 0x21 || report[0] == 0x30) && length < kStandardInput) ? kStandardInput : length;
        uint8_t *bytes = static_cast<uint8_t *>(input->getBytesNoCopy());
        memcpy(bytes, report, length);
        memset(bytes + length, 0, size - length);
        input->setLength(size);
        uint64_t ts = timestamp ? timestamp : mach_absolute_time();
        if (ts <= lastReportTime)
            ts = lastReportTime + 1;
        lastReportTime = ts;
        return handleReportWithTime(ts, input, 0 /* kIOHIDReportTypeInput */, 0);
    }
};

OSDefineMetaClassAndStructors(SwitchJoyConDevice, IOHIDDevice)

    extern "C" __attribute__((used)) IOReturn
    switch_joycon_set_report(SwitchJoyConDevice *device, IOMemoryDescriptor *report, uint32_t type,
                             IOOptionBits options)
{
    return device->sendReport(report, type, options);
}

__asm__(".text\n"
        ".globl __ZN18SwitchJoyConDevice9opaque183Ev\n"
        ".p2align 2\n"
        "__ZN18SwitchJoyConDevice9opaque183Ev:\n"
        "  b _switch_joycon_set_report\n");

// Rail Joy-Cons' Home (R) and Capture (L) as Consumer Menu and Snapshot. Shares one rail side's
// IOWorkLoop; only called on button edges, serialized on `loop`.
// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage (its kalloc type view)
class SwitchJoyConButtons : public IOHIDDevice {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchJoyConButtons) IOWorkLoop *loop = nullptr;
    IOBufferMemoryDescriptor *report = nullptr;
    uint64_t lastReportTime = 0;
    uint8_t held[2] = {};  // 0 R (Menu), 1 L (Snapshot)
    uint8_t posted = 0;

    void applyLocked(bool isLeft, bool down, uint64_t timestamp)
    {
        const uint8_t bit = isLeft ? kSnapshot : kMenu;
        held[isLeft ? 1 : 0] = down ? bit : 0u;
        const uint8_t now = static_cast<uint8_t>(held[0] | held[1]);
        if (now == posted)
            return;
        if ((now & kMenu) && !(posted & kMenu))
            switch_wake_event(SWITCH_WAKE_RAIL_HOME);
#if SWITCH_RECORDER
        if ((now & kSnapshot) && !(posted & kSnapshot))
            switch_recorder_mark();
#endif
        posted = now;
        uint8_t *bytes = static_cast<uint8_t *>(report->getBytesNoCopy());
        bytes[0] = 1;
        bytes[1] = now;
        report->setLength(2);
        uint64_t ts = timestamp ? timestamp : mach_absolute_time();
        if (ts <= lastReportTime)
            ts = lastReportTime + 1;
        lastReportTime = ts;
        handleReportWithTime(ts, report, 0 /* kIOHIDReportTypeInput */, 0);
    }

public:
    bool initWithLoop(IOWorkLoop *sharedLoop)
    {
        if (!sharedLoop || !init())
            return false;
        loop = sharedLoop;
        loop->retain();
        return true;
    }

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
        return OSNumber::withNumber(kButtonsProduct, 32);
    }
#endif

    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override
    {
        *out = IOBufferMemoryDescriptor::withBytes(kButtonsDescriptor, sizeof(kButtonsDescriptor), kIODirectionNone);
        return *out ? kIOReturnSuccess : kIOReturnNoMemory;
    }

    bool start(IOService *provider) override
    {
        report = IOBufferMemoryDescriptor::withCapacity(2, kIODirectionNone);
        if (!loop || !report)
            return false;
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

    void update(bool isLeft, bool down, uint64_t timestamp)
    {
        if (loop->inGate()) {
            applyLocked(isLeft, down, timestamp);
            return;
        }
        ButtonsEdgeArgs args = {timestamp, isLeft, down};
        loop->runAction(
            [](OSObject *owner, void *arg, void *, void *, void *) -> IOReturn {
                const auto *a = static_cast<const ButtonsEdgeArgs *>(arg);
                static_cast<SwitchJoyConButtons *>(owner)->applyLocked(a->isLeft, a->down, a->timestamp);
                return kIOReturnSuccess;
            },
            this, &args);
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

static void removeAppleNintendoEntries(uint32_t productID)
{
    OSNumber *vendor = OSNumber::withNumber(kNintendo, 32);
    OSNumber *product = OSNumber::withNumber(productID, 32);
    OSString *publisher = OSString::withCString(kAppleGCBundle);
    const char *idKeys[] = {"VendorID", "ProductID"};
    const OSObject *idValues[] = {vendor, product};
    OSDictionary *ids = dictionary(idKeys, idValues, 2);
    const char *dummyKeys[] = {kIOPersonalityPublisherKey, "VendorID", "ProductID"};
    const OSObject *dummyValues[] = {publisher, vendor, product};
    OSDictionary *dummy = dictionary(dummyKeys, dummyValues, 3);
    const char *mergerKeys[] = {kIOPersonalityPublisherKey, "IOPropertyMatch"};
    const OSObject *mergerValues[] = {publisher, ids};
    OSDictionary *merger = dictionary(mergerKeys, mergerValues, 2);
    if (!dummy || !merger)
        panic("SwitchJoyCon: no memory for a catalogue lookup");
    gIOCatalogue->removeDrivers(dummy, false);
    gIOCatalogue->removeDrivers(merger, false);
    OSObject *all[] = {vendor, product, publisher, ids, dummy, merger};
    for (const OSObject *object : all) {
        if (object)
            object->release();
    }
}
#endif

void switch_joycon_hid_select_personalities()
{
#if !XNU_12377_121_6
    static bool done;
    if (done)
        return;
    done = true;
    for (const uint32_t product : kNintendoProducts)
        removeAppleNintendoEntries(product);
#endif
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

static SwitchJoyConButtons *gButtons;  // initialized once at startup before either rail enters detection

void switch_joycon_hid_buttons_init(IOWorkLoop *loop)
{
    if (__atomic_load_n(&gButtons, __ATOMIC_ACQUIRE))
        return;
    switch_joycon_hid_prepare();
    IOService *resources = IOService::getResourceService();
    auto *buttons = OSTypeAlloc(SwitchJoyConButtons);
    if (!resources || !buttons || !buttons->initWithLoop(loop) || !buttons->attach(resources))
        panic("SwitchJoyCon: system-buttons HID device: attach failed");
    if (!buttons->start(resources))
        panic("SwitchJoyCon: system-buttons HID device: IOHIDDevice::start failed");
    __atomic_thread_fence(__ATOMIC_RELEASE);
    gButtons = buttons;  // kept for the boot
}

void switch_joycon_hid_buttons_edge(bool left, bool down, uint64_t timestamp)
{
    if (SwitchJoyConButtons *buttons = __atomic_load_n(&gButtons, __ATOMIC_ACQUIRE))
        buttons->update(left, down, timestamp);
}

void switch_joycon_hid_buttons_release(bool left)
{
    if (SwitchJoyConButtons *buttons = __atomic_load_n(&gButtons, __ATOMIC_ACQUIRE))
        buttons->update(left, false, 0);
}

IOService *switch_joycon_hid_create(IOWorkLoop *loop, bool left, const uint8_t mac[6], uint32_t generation,
                                    SwitchJoyConOutput output, SwitchJoyConSessionEnded ended, void *context)
{
    switch_joycon_hid_prepare();
    IOService *resources = IOService::getResourceService();
    auto *device = OSTypeAlloc(SwitchJoyConDevice);
    if (!resources || !device || !device->initWith(loop, left, mac, generation, output, ended, context) ||
        !device->attach(resources)) {
        panic("SwitchJoyCon: Joy-Con (%c) HID device: attach failed", left ? 'L' : 'R');
    }
    if (!device->start(resources))
        panic("SwitchJoyCon: Joy-Con (%c) HID device: IOHIDDevice::start failed", left ? 'L' : 'R');
    return device;  // creator's reference, dropped by switch_joycon_hid_destroy
}

IOReturn switch_joycon_hid_input(const IOService *device, const uint8_t *report, size_t length, uint64_t timestamp)
{
    auto *joycon = OSDynamicCast(SwitchJoyConDevice, device);
    if (!joycon)
        return kIOReturnBadArgument;
    return joycon->deliver(report, length, timestamp);
}

void switch_joycon_hid_destroy(const IOService *device)
{
    auto *joycon = OSDynamicCast(SwitchJoyConDevice, device);
    if (!joycon)
        return;
    joycon->detachOutput();
    joycon->terminate();
    // NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): drops switch_joycon_hid_create's reference
    joycon->release();
}
