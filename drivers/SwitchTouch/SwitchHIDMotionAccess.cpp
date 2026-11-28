// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Motion & Fitness gate on the motion services, as J181 has it on 24B119:
//  * J181's AppleSPUHIDDriver (24B119 0xfffffff0090ad7a0) sets motionRestrictedService on each of its services, true
//    for one that dispatches accelerometer, gyroscope or combined motion events (its dispatchAccel, dispatchGyro and
//    dispatchCombo) or is AppleVendorMotion 0xff0c usage 4, 5, 7 or 8, and gives those TCCService
//    kTCCServiceMotionSensors. Ours are the accelerometer, gyroscope and device motion services.
//  * IOKit's IOHIDEventSystem reads TCCService once, when it adds the service (IOKit 0x18f5aeb34). It finds services by
//    kIOFirstMatchNotification on HIDServiceSupport (0x18f612290), which IOService::doServiceMatch delivers after the
//    first-publish handlers, so the property is set from the event driver's first publish.
//  * It then matches the service for a connection only if the connection's code signing status has CS_VALID and
//    CS_PLATFORM_BINARY without CS_PLATFORM_PATH, or TCC answers right 2 for it (0x18f5910b8-0x18f5911c4; it logs
//    "denied match of service" otherwise). AMFI makes a binary whose CDHash is in the static trust cache a platform
//    binary, and the boot package's static module carries every translated System executable's and every deploy
//    update's (boot/defs.bzl), so locationd, backboardd and SpringBoard are expected to pass; no device log has shown
//    their status yet, and tools/devicerun/motion.py's motion.tcc-denied fails a boot where one is refused. HW Test
//    reads motion through CoreMotion, so through locationd.
//  * No image in 24B119's dyld cache names motionRestrictedService; it is published as J181 publishes it.
//  * 21H461's driver sets motionRestrictedService and has no TCCService; this file builds to nothing there.
#include "SwitchHIDMotionAccess.h"

#if SWITCH_IPSW_KIND_24B119
namespace {
IOService *devices[3];
unsigned count;
IONotifier *notifier;

bool published(void *, void *, IOService *driver, IONotifier *)
{
    const IOService *interface = driver ? driver->getProvider() : nullptr;
    const IOService *device = interface ? interface->getProvider() : nullptr;
    const unsigned known = __atomic_load_n(&count, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; device && i < known; ++i) {
        if (devices[i] != device)
            continue;
        driver->setProperty("motionRestrictedService", true);
        driver->setProperty("TCCService", "kTCCServiceMotionSensors");
    }
    return true;
}
}  // namespace

void switch_hid_motion_restrict(IOService *device)
{
    const unsigned at = __atomic_load_n(&count, __ATOMIC_RELAXED);
    if (at == sizeof(devices) / sizeof(devices[0])) {
        IOLog("SwitchHID: no Motion & Fitness gate on a fourth motion service\n");
        return;
    }
    devices[at] = device;
    __atomic_store_n(&count, at + 1, __ATOMIC_RELEASE);
    if (notifier)
        return;
    OSDictionary *matching = IOService::serviceMatching("IOHIDEventDriver");
    if (matching)
        notifier = IOService::addMatchingNotification(gIOFirstPublishNotification, matching, &published, nullptr);
    OSSafeReleaseNULL(matching);
    if (!notifier)
        IOLog("SwitchHID: no Motion & Fitness gate on the motion services: no publish notification\n");
}
#endif
