// SPDX-License-Identifier: BSD-2-Clause-Patent
// The USB-C port's IOPort and its transports (SwitchUSBPort.h).
#include "SwitchUSBPort.h"
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <kern/thread_call.h>
#if DEVELOPMENT || DEBUG
#include <os/log.h>
#endif
#include "OriginalIOPort.h"
#include "SwitchUSB.h"
#include "XusbPortTransports.h"

namespace {
constexpr const char *kPortPath = "/arm-io/port-usb-c-1";
constexpr int kDataRoleDevice = 1, kDataRoleHost = 2;

thread_call_t gRoleCall;
IOService *gPort;  // written once by the notification, read by the role thread

// The notifier for the device a retired host role's transports wait on (Transports::retire). Role thread, except the
// notifier, which the termination handler can take first.
bool gRetiredDeviceGone;
IONotifier *gDeviceNotifier;

#if DEVELOPMENT || DEBUG
bool gLaneWasUp;  // role thread
#endif

void *createTransport(uint32_t type, enum xusb_port_role role)
{
    const int dataRole = role == XUSB_PORT_HOST ? kDataRoleHost : kDataRoleDevice;
    void *transport = nullptr;
    if (type == xport::kUSB3) {
        transport = switch_original_port_usb3_transport();
        if (transport)
            switch_original_port_usb3_set_data_role(transport, dataRole);
    } else {
        transport = switch_original_port_usb2_transport();
        if (transport)
            switch_original_port_usb2_set_data_role(transport, dataRole);
    }
    return transport;
}

bool addTransport(void *transport, uint32_t type, uint32_t index)
{
    if (switch_original_port_add_transport(gPort, transport, index))
        return true;
    IOLog("SwitchUSB: USB-C port refused a USB %u transport\n", type == xport::kUSB3 ? 3u : 2u);
    return false;
}

void removeTransport(uint32_t type, uint32_t index)
{
    const int status = switch_original_port_remove_transport(gPort, type, index);
    if (status != kIOReturnSuccess)
        IOLog("SwitchUSB: USB-C port kept its USB %u transport (%#x)\n", type == xport::kUSB3 ? 3u : 2u, status);
}

void releaseTransport(void *transport)
{
    static_cast<OSObject *>(transport)->release();
}

#if DEVELOPMENT || DEBUG
void noteTransport(const char *what, uint32_t type, uint32_t index, enum xusb_port_role role, const char *why)
{
    os_log(OS_LOG_DEFAULT, "SwitchUSB: USB-C port transport %s: USB %u, index %u, data role %s (%s)", what,
           type == xport::kUSB3 ? 3u : 2u, index, xusb_port_role_name(role), why ? why : "");
}
#else
void noteTransport(const char *, uint32_t, uint32_t, enum xusb_port_role, const char *)
{}
#endif

constexpr xport::Ops kPortOps = {&createTransport, &addTransport, &removeTransport, &releaseTransport, &noteTransport};
xport::Transports gTransports(kPortOps);  // role thread

// NOLINTNEXTLINE(misc-const-correctness): IOServiceMatchingNotificationHandler's signature
bool portPublished(void *, void *, IOService *port, IONotifier *notifier)
{
#if DEVELOPMENT || DEBUG
    const auto *type = OSDynamicCast(OSNumber, port->getProperty("PortType"));
    const auto *number = OSDynamicCast(OSNumber, port->getProperty("PortNumber"));
    os_log(OS_LOG_DEFAULT, "SwitchUSB: USB-C port %s@%s found (port type %d, port number %d)", port->getName(),
           port->getLocation() ? port->getLocation() : "", type ? static_cast<int>(type->unsigned32BitValue()) : -1,
           number ? static_cast<int>(number->unsigned32BitValue()) : -1);
#endif
    port->retain();
    __atomic_store_n(&gPort, port, __ATOMIC_RELEASE);
    // NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): gPort holds that reference for the driver's life
    notifier->remove();
    thread_call_enter(gRoleCall);
    return true;
}

// NOLINTNEXTLINE(misc-const-correctness): IOServiceMatchingNotificationHandler's signature
bool deviceTerminated(void *, void *, IOService *, IONotifier *)
{
    if (IONotifier *notifier = __atomic_exchange_n(&gDeviceNotifier, nullptr, __ATOMIC_ACQ_REL))
        notifier->remove();
    __atomic_store_n(&gRetiredDeviceGone, true, __ATOMIC_RELEASE);
    thread_call_enter(gRoleCall);
    return true;
}

void dropRetired()
{
    if (IONotifier *notifier = __atomic_exchange_n(&gDeviceNotifier, nullptr, __ATOMIC_ACQ_REL))
        notifier->remove();
    gTransports.dropRetired();
}

// False when the device has already gone: the transports can come out at once.
bool watchUntilGone(IOService *device)
{
    __atomic_store_n(&gRetiredDeviceGone, false, __ATOMIC_RELEASE);
    OSDictionary *matching = IOService::registryEntryIDMatching(device->getRegistryEntryID());
    IONotifier *notifier =
        matching ? IOService::addMatchingNotification(gIOTerminatedNotification, matching, &deviceTerminated, nullptr)
                 : nullptr;
    OSSafeReleaseNULL(matching);
    if (!notifier || device->isInactive()) {
        if (notifier)
            notifier->remove();
        return false;
    }
    __atomic_store_n(&gDeviceNotifier, notifier, __ATOMIC_RELEASE);
    if (__atomic_load_n(&gRetiredDeviceGone, __ATOMIC_ACQUIRE) &&
        __atomic_exchange_n(&gDeviceNotifier, nullptr, __ATOMIC_ACQ_REL)) {
        notifier->remove();  // the handler ran before the notifier was stored
    }
    return true;
}

bool watchActiveDevice()
{
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        IOService *device = switchxhci_copy_root_device();
        if (!device)
            return false;
        const bool watched = watchUntilGone(device);
        OSSafeReleaseNULL(device);
        if (watched)
            return true;
    }
    return false;
}

// Leaving the host role for nothing attached with a device still on the root port or behind a hub.
bool retiresForDevice(enum xusb_port_role role)
{
    if (role != XUSB_PORT_EMPTY || gTransports.role() != XUSB_PORT_HOST)
        return false;
    return watchActiveDevice();
}
}  // namespace

bool switch_usb_port_watch(thread_call_t roleCall)
{
    gRoleCall = roleCall;
    IORegistryEntry *node = IORegistryEntry::fromPath(kPortPath, gIODTPlane);
    if (!node) {
        IOLog("SwitchUSB: no %s: the USB-C port has no IOPort\n", kPortPath);
        return false;
    }
    OSDictionary *matching = IOService::serviceMatching("IOPort");
    const OSDictionary *parent = IOService::registryEntryIDMatching(node->getRegistryEntryID());
    const IONotifier *notifier = nullptr;
    if (matching && parent && matching->setObject(kIOParentMatchKey, parent))
        notifier = IOService::addMatchingNotification(gIOFirstPublishNotification, matching, &portPublished, nullptr);
    OSSafeReleaseNULL(parent);
    OSSafeReleaseNULL(matching);
    node->release();
    return notifier != nullptr;
}

void switch_usb_port_follow(enum xusb_port_role role, bool laneUp, bool superSpeedReset)
{
    if (!__atomic_load_n(&gPort, __ATOMIC_ACQUIRE))
        return;
    if (gTransports.holdsRetired()) {
        if (role != XUSB_PORT_EMPTY) {
            dropRetired();
        } else if (__atomic_load_n(&gRetiredDeviceGone, __ATOMIC_ACQUIRE) && !watchActiveDevice()) {
            dropRetired();
        }
    }
    const char *why = nullptr;
#if DEVELOPMENT || DEBUG
    why = xusb_port_change_reason(gTransports.role(), role, laneUp, gLaneWasUp, superSpeedReset);
    gLaneWasUp = laneUp;
#endif
    if (retiresForDevice(role)) {
        gTransports.retire(why);
        return;
    }
    gTransports.follow(role, laneUp, superSpeedReset, why);
}

bool switch_usb_port_holding_retired()
{
    return gTransports.holdsRetired();
}
