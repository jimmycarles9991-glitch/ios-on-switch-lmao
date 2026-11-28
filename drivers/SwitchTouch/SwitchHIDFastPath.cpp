// SPDX-License-Identifier: BSD-2-Clause-Patent
// CoreMotion's fast path on the device motion service (SwitchHID.h). On an iPad, CLSensorFusionServiceSPU opens it with
// IOHIDServiceClientFastPathInit (IOHIDLib.plugin's IOHIDEventServiceFastPathClass, plug-in type 0B842ADD-..., which
// opens IOHIDEventServiceFastPathUserClient) and reads the latest sample through it; without it CoreMotion logs
// "[FastPath] openFastPathPrivate() failed" at every start and takes the pushed sample. IOKit's
// IOCreatePlugInInterfaceForService reads IOCFPlugInTypes from the service itself (IORegistryEntryCreateCFProperty), and
// IOHIDEventService's defaults refuse the user client (openForClient false, copyEventForClient NULL), so the service's
// event driver gets the plug-in type and its own vtable copy:
//  * openForClient (reserved 18) admits the client as J181's provider does (AppleSPUHIDDriver, 24B119
//    0xfffffff0090b0c1c). The user client puts FastPathHasEntitlement and FastPathMotionEventEntitlement in the open
//    properties (21H461 0xfffffff005ad1d14, 0xfffffff005ad1d40). The provider reads the first only when the open asks
//    for "aonBuffer", the AOP's always-on sample history, and refuses that open without it. There is no such history
//    here, so every aonBuffer open is refused. It records the second for its MotionEventRestricted state, in which
//    copies to a client without it return NULL (0xfffffff0090d704c). Nothing in 24B119's user space sets that state: no
//    binary in the System cryptex's caches or under /usr and /System/Library/CoreServices names it, nor aonBuffer.
//    21H461's kernelcache carries the same keys; its branches were not read. Which processes reach the user client is
//    the sandbox's call (tools/devicerun/motion.py);
//  * copyEventForClient (reserved 15) answers the latest report's event, retained; the user client copies it into the
//    shared memory its client reads. None between sessions (switch_hid_forget_latest): CoreMotion then logs a failed
//    copy and takes the pushed sample (21H461's getter, locationd 0x100528874), which beats one from the session before.
//    Debug kernels count the answers (switch_hid_fastpath_copies);
//  * closeForClient (reserved 19) has nothing to undo;
//  * dispatchEvent (reserved 8, as the touch edge wrapper's) dispatches the event, then keeps it as the latest, so a
//    copy never answers an event the event system has not had yet.
// Both builds' user clients call the three through these slots, with the provider at +0xe0 and the context at +0xe8:
// 21H461 IOHIDFamily open 0xfffffff005ad1d64, copy 0xfffffff005ad1ef4, close 0xfffffff005ad1e8c; 24B119 open
// 0xfffffff0099265d0, copy 0xfffffff0098cc338, close 0xfffffff0098cc294 (their PAC diversifiers are the vtable's
// slots 208, 205 and 209). Slots 199 and 200 must answer the device's 0xff0c / 5 first, as the temperature hook checks.
#include "SwitchHIDFastPath.h"
#include <stdint.h>
#include <string.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include "SwitchHID.h"

namespace {
// IOHIDEventService's reserved slot n is vtable slot 190 + n in both builds' IOHIDFamily: 21H461's from its
// IOHIDFamily-2031.120.4 header with slot 198 (reserved 8) proven on hardware, 24B119's read from its kernelcache's
// IOHIDEventDriver vtable (205-209 IOHIDEventService's defaults, 198 and 201 IOHIDEventDriver's own).
#if SWITCH_IPSW_KIND_24B119 || SWITCH_IPSW_KIND_21H461
enum : unsigned {
    kSlots = 256,
    kDispatch = 198,
    kPrimaryUsagePage = 199,
    kPrimaryUsage = 200,
    kCopyEventForClient = 205,
    kCopyPropertyForClient = 206,
    kOpenForClient = 208,
    kCloseForClient = 209,
};

// IOHIDEventServiceFastPathUserClient::getSharedMemorySize queries QueueSize via slot 206 and refuses opens without
// an OSNumber >= 5.
constexpr uint32_t kQueueSize = 4096;

// IOHIDLib.plugin's CFPlugInTypes maps the type to its factory; the path is the one IOHIDFamily's embedded
// IOHIDEventDriver personality gives its other IOHIDLib types.
constexpr const char *kPlugInType = "0B842ADD-C395-4352-95F6-6585EB515623";  // kIOHIDServiceFastPathPlugInTypeID
constexpr const char *kPlugIn = "IOHIDFamily.kext/PlugIns/IOHIDLib.plugin";
#else
#error "SwitchHIDFastPath: no IOHIDEventService slots for this IPSW kind"
#endif

IOService *motionDevice;
IONotifier *notifier;
uintptr_t *original;  // the event driver class's vtable, and the copy every motion event driver of the boot gets
uintptr_t *table;
uintptr_t originalDispatch;
uintptr_t originalCopyProperty;
// copyEventForClient runs on the user client's thread, not the event driver's work loop (IOHIDEventService.h), while
// dispatchEvent replaces the event on the work loop: both run in thread context, never primary IRQ.
IOLock *lock;
OSObject *latest;
#if DEVELOPMENT || DEBUG
uint64_t copies;
#endif

template <typename R, typename... A> R call(void *object, unsigned slot, A... args)
{
    return reinterpret_cast<R (*)(void *, A...)>((*reinterpret_cast<uintptr_t **>(object))[slot])(object, args...);
}

void keep(OSObject *event)
{
    if (!event && !__atomic_load_n(&latest, __ATOMIC_RELAXED))
        return;
    if (event)
        event->retain();
    IOLockLock(lock);
    const OSObject *replaced = latest;
    __atomic_store_n(&latest, event, __ATOMIC_RELAXED);
    IOLockUnlock(lock);
    OSSafeReleaseNULL(replaced);
}

// NOLINTBEGIN(misc-const-correctness): IOHIDEventService's slots, their parameters as declared
void dispatch(IOService *driver, OSObject *event, IOOptionBits options)
{
    reinterpret_cast<void (*)(IOService *, OSObject *, IOOptionBits)>(originalDispatch)(driver, event, options);
    keep(event);
}

OSObject *copyEventForClient(IOService *, OSObject *, IOOptionBits, void *)
{
    if (!__atomic_load_n(&latest, __ATOMIC_RELAXED))
        return nullptr;
    IOLockLock(lock);
    OSObject *event = latest;
    if (event) {
        event->retain();
#if DEVELOPMENT || DEBUG
        __atomic_store_n(&copies, copies + 1, __ATOMIC_RELAXED);
#endif
    }
    IOLockUnlock(lock);
    return event;
}

OSObject *copyPropertyForClient(const IOService *driver, const char *key, void *context)
{
    if (key && strcmp(key, "QueueSize") == 0)
        return OSNumber::withNumber(kQueueSize, 32);
    return reinterpret_cast<OSObject *(*)(const IOService *, const char *, void *)>(originalCopyProperty)(driver, key,
                                                                                                          context);
}

bool openForClient(IOService *, IOService *, IOOptionBits, OSDictionary *properties, void **context)
{
    if (context)
        *context = nullptr;
    const auto *history = properties ? OSDynamicCast(OSBoolean, properties->getObject("aonBuffer")) : nullptr;
    return !(history != nullptr && history->isTrue());
}

void closeForClient(IOService *, IOService *, void *, IOOptionBits)
{}

// NOLINTEND(misc-const-correctness)

void publishPlugIn(IOService *driver)
{
    const auto *types = OSDynamicCast(OSDictionary, driver->getProperty("IOCFPlugInTypes"));
    OSDictionary *merged =
        types ? OSDictionary::withDictionary(types, types->getCount() + 1) : OSDictionary::withCapacity(1);
    const OSString *plugIn = OSString::withCString(kPlugIn);
    if (merged && plugIn && merged->setObject(kPlugInType, plugIn))
        driver->setProperty("IOCFPlugInTypes", merged);
    OSSafeReleaseNULL(plugIn);
    OSSafeReleaseNULL(merged);
}

// The lock and the vtable copy, once a boot: the copy outlives every driver that uses it.
bool makeTable(uintptr_t *vtable)
{
    IOLock *created = IOLockAlloc();
    auto *copy = static_cast<uintptr_t *>(IOMalloc(kSlots * sizeof(uintptr_t)));
    if (!created || !copy) {
        if (created)
            IOLockFree(created);
        if (copy)
            IOFree(copy, kSlots * sizeof(uintptr_t));
        IOLog("SwitchHID: device motion fast path off: no memory\n");
        return false;
    }
    memcpy(copy, vtable, kSlots * sizeof(uintptr_t));
    originalDispatch = copy[kDispatch];
    originalCopyProperty = copy[kCopyPropertyForClient];
    copy[kDispatch] = reinterpret_cast<uintptr_t>(&dispatch);
    copy[kCopyEventForClient] = reinterpret_cast<uintptr_t>(&copyEventForClient);
    copy[kCopyPropertyForClient] = reinterpret_cast<uintptr_t>(&copyPropertyForClient);
    copy[kOpenForClient] = reinterpret_cast<uintptr_t>(&openForClient);
    copy[kCloseForClient] = reinterpret_cast<uintptr_t>(&closeForClient);
    __atomic_store_n(&lock, created, __ATOMIC_RELEASE);
    original = vtable;
    table = copy;
    return true;
}

bool install(IOService *driver)
{
    const uint32_t page = call<uint32_t>(driver, kPrimaryUsagePage);
    const uint32_t usage = call<uint32_t>(driver, kPrimaryUsage);
    if (page != 0xff0c || usage != 5) {
        IOLog("SwitchHID: device motion fast path off: event driver slots 199/200 answer %#x/%#x, not 0xff0c/0x5 "
              "(IOHIDEventService's reserved slots moved)\n",
              page, usage);
        return false;
    }
    uintptr_t *const vtable = *reinterpret_cast<uintptr_t **>(driver);
    if (table && vtable != original) {
        IOLog("SwitchHID: device motion fast path off: a new event driver of another class\n");
        return false;
    }
    if (!table && !makeTable(vtable))
        return false;
    keep(nullptr);
    publishPlugIn(driver);
    driver->setProperty("NativeSandboxGrantProcess", "backboardd,SpringBoard,locationd");
    __atomic_thread_fence(__ATOMIC_RELEASE);
    *reinterpret_cast<uintptr_t **>(driver) = table;
#if DEVELOPMENT || DEBUG
    IOLog("SwitchHID: device motion fast path on (event driver slots 199/200 answer 0xff0c/0x5)\n");
#endif
    return true;
}

// IOKit's first publish of each IOHIDEventDriver, on the matching thread before the event system learns of the
// service, so CoreMotion never sees the service without the plug-in type. A motion event driver published again (a
// re-match) is hooked again.
bool published(void *, void *, IOService *driver, IONotifier *)
{
    const IOService *interface = driver ? driver->getProvider() : nullptr;
    if (interface && interface->getProvider() == motionDevice && *reinterpret_cast<uintptr_t **>(driver) != table)
        (void)install(driver);
    return true;
}
}  // namespace

void switch_hid_fastpath_watch(IOService *device)
{
    motionDevice = device;
    OSDictionary *matching = IOService::serviceMatching("IOHIDEventDriver");
    if (matching)
        notifier = IOService::addMatchingNotification(gIOFirstPublishNotification, matching, &published, nullptr);
    OSSafeReleaseNULL(matching);
    if (!notifier)
        IOLog("SwitchHID: device motion fast path off: no publish notification for its event driver\n");
}

void switch_hid_forget_latest(const IOService *device)
{
    if (device == motionDevice && __atomic_load_n(&lock, __ATOMIC_ACQUIRE))
        keep(nullptr);
}

#if DEVELOPMENT || DEBUG
uint64_t switch_hid_fastpath_copies()
{
    return __atomic_load_n(&copies, __ATOMIC_RELAXED);
}
#endif
