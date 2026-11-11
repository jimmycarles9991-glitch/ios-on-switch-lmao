// SPDX-License-Identifier: BSD-2-Clause-Patent
// The radio as IONetworkingFamily drivers are: IO80211Controller, an IOEthernetController, and its IO80211Interface,
// an IOEthernetInterface. IONetworkStack names the interface when configd's interface namer asks and attaches it to
// the stack, as it does every iPad's network interfaces; the family owns the ifnet, its output queue and start thread,
// the input queue and the packet taps. The class names are the ones SystemConfiguration looks for (a controller
// conforming to IO80211Controller is Wi-Fi); IO80211.framework and wifid find the interface by its IOInterfaceName and
// IO80211InterfaceRole properties. Neither 21H461 nor 24B119 ships an IO80211 family, so nothing else defines them.
//
// The classes' superclasses live in the prelinked IONetworkingFamily, so this is a late object (kernel_driver late):
// its static initializer runs from publish() once the family is loaded. Their vtables are checked against
// kits/SwitchABI's IOEthernetController and IOEthernetInterface tables (BUILD.bazel's family bindings).
#include "OriginalIOEthernetController.h"
#include "OriginalIOEthernetInterface.h"
#include <IOKit/IOBSD.h>
#include <IOKit/IOLib.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <net/kpi_interface.h>
#include "SwitchWLANApple80211.h"
#include "SwitchWLANNet.h"

extern "C" OSReturn switch_kernel_classes(void (*initialize)());
extern "C" void switch_wlan_family_static_init(void) __asm__("__GLOBAL__sub_I_SwitchWLANFamily.cpp");

namespace {

// IONetworkController.h and IONetworkMedium.h (IONetworkingFamily 3.4)
constexpr UInt32 kIOPacketFilterMulticastAll = 0x20, kIOPacketFilterPromiscuous = 0x100;
constexpr char kIONetworkFilterGroup[] = "IONetworkFilterGroup";

// The radio's work loop, which IONetworkController::start takes for the controller's command gate: the frames it
// receives come from that loop, so the interface's input queue is fed from one thread as the family requires.
IOWorkLoop *gLoop;

}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class IO80211Controller : public IOEthernetController {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(IO80211Controller);

public:
    IOWorkLoop *getWorkLoop() const override
    {
        return gLoop;
    }

    IOReturn getHardwareAddress(IOEthernetAddress *address) override
    {
        memcpy(address->bytes, mac_, sizeof(mac_));
        return kIOReturnSuccess;
    }

    IONetworkInterface *createInterface() override;
    bool configureInterface(IONetworkInterface *interface) override;

    IOReturn enable(IONetworkInterface *interface) override
    {
        return swlan::net::enable(reinterpret_cast<IOEthernetInterface *>(interface));
    }

    IOReturn disable(IONetworkInterface *interface) override
    {
        return kIOReturnSuccess;
    }

    IOReturn outputStart(IONetworkInterface *interface, IOOptionBits options) override
    {
        return swlan::net::output(reinterpret_cast<IOEthernetInterface *>(interface));
    }

    IOReturn getPacketFilters(const OSSymbol *group, UInt32 *filters) const override
    {
        const IOReturn r = IOEthernetController::getPacketFilters(group, filters);
        if (r == kIOReturnSuccess && group->isEqualTo(kIONetworkFilterGroup))
            *filters |= kIOPacketFilterMulticastAll;
        return r;
    }

    IOReturn enablePacketFilter(const OSSymbol *group, UInt32 filter, UInt32 enabled, IOOptionBits options) override
    {
        if (group->isEqualTo(kIONetworkFilterGroup) && filter == kIOPacketFilterMulticastAll)
            return setAll(kIOPacketFilterMulticastAll, true);
        return IOEthernetController::enablePacketFilter(group, filter, enabled, options);
    }

    IOReturn disablePacketFilter(const OSSymbol *group, UInt32 filter, UInt32 enabled, IOOptionBits options) override
    {
        if (group->isEqualTo(kIONetworkFilterGroup) && filter == kIOPacketFilterMulticastAll)
            return setAll(kIOPacketFilterMulticastAll, false);
        return IOEthernetController::disablePacketFilter(group, filter, enabled, options);
    }

    IOReturn setMulticastMode(bool active) override
    {
        return kIOReturnSuccess;
    }

    IOReturn setMulticastList(IOEthernetAddress *addresses, UInt32 count) override
    {
        static_assert(sizeof(IOEthernetAddress) == 6, "IOEthernetAddress");
        swlan::net::multicast(reinterpret_cast<const uint8_t (*)[6]>(addresses), count, all_ != 0);
        return kIOReturnSuccess;
    }

    IOReturn setPromiscuousMode(bool active) override
    {
        return setAll(kIOPacketFilterPromiscuous, active);
    }

    void setMac(const uint8_t mac[6])
    {
        memcpy(mac_, mac, sizeof(mac_));
    }

private:
    uint8_t mac_[6];
    UInt32 all_ = 0;  // the filters that want every group: multicast-all (IFF_ALLMULTI) and promiscuous

    // The firmware filters multicast by its group list or takes every group; it has no promiscuous mode in station
    // operation, so promiscuous gets every group too.
    IOReturn setAll(UInt32 filter, bool on)
    {
        all_ = on ? (all_ | filter) : (all_ & ~filter);
        swlan::net::multicast_all(all_ != 0);
        return kIOReturnSuccess;
    }
};

OSDefineMetaClassAndStructors(IO80211Controller, IOEthernetController);

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class IO80211Interface : public IOEthernetInterface {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(IO80211Interface);

public:
    // IO80211.framework and wifid find an 802.11 interface's service by IOPropertyMatch on IOInterfaceName, which
    // Apple's IO80211 interfaces publish beside the "BSD Name" IONetworkStack sets before this attach.
    IOReturn attachToDataLinkLayer(IOOptionBits options, void *parameter) override
    {
        const IOReturn r = IOEthernetInterface::attachToDataLinkLayer(options, parameter);
        if (r != kIOReturnSuccess)
            return r;
        if (OSObject *name = OSDynamicCast(OSString, getProperty(kIOBSDNameKey)))
            setProperty("IOInterfaceName", name);
        ifnet_t ifp = getIfnet();
        IOLog("SwitchWLAN: %s%u attached by IONetworkStack (IO80211Interface on IOEthernetInterface)\n",
              ifnet_name(ifp), ifnet_unit(ifp));
        return r;
    }

    // wifid's Apple80211 requests and the 802.11 media come as ioctls on the interface; the rest are the family's.
    SInt32 performCommand(IONetworkController *controller, unsigned long cmd, void *arg0, void *arg1) override
    {
        switch (cmd) {
        case SIOCGA80211:
        case SIOCSA80211:
            return swlan::net::apple80211(cmd == SIOCSA80211, arg1);
        case SIOCGIFMEDIA64:  // (the stack hands SIOCGIFXMEDIA over as this too)
            return swlan::net::media(arg1);
        default:
            return IOEthernetInterface::performCommand(controller, cmd, arg0, arg1);
        }
    }
};

OSDefineMetaClassAndStructors(IO80211Interface, IOEthernetInterface);

IONetworkInterface *IO80211Controller::createInterface()
{
    IO80211Interface *interface = OSTypeAlloc(IO80211Interface);
    if (interface && !interface->init(reinterpret_cast<IONetworkController *>(this)))
        OSSafeReleaseNULL(interface);
    if (interface)
        interface->setProperty("IO80211InterfaceRole", "Infrastructure");
    return reinterpret_cast<IONetworkInterface *>(interface);
}

bool IO80211Controller::configureInterface(IONetworkInterface *interface)
{
    if (!IOEthernetController::configureInterface(interface))
        return false;
    auto *ethernet = reinterpret_cast<IOEthernetInterface *>(interface);
    // The stack's output queue (512 frames, its AQM and flow control deciding what waits) and its start thread calling
    // outputStart, which takes what the radio has DMA buffers for. r8a: with frames past the radio's 32 buffers
    // dropped instead, 40% of en0's frames were lost at 40 Mb/s.
    constexpr uint32_t kSendQueue = 512;
    if (ethernet->configureOutputPullModel(0, 0, kSendQueue, 0) != kIOReturnSuccess) {
        IOLog("SwitchWLAN: en0's output queue was refused\n");
        return false;
    }
    if (!ethernet->setInterfaceSubType(IFNET_SUBFAMILY_WIFI)) {
        IOLog("SwitchWLAN: en0's Wi-Fi subfamily was refused\n");
        return false;
    }
    return true;
}

namespace {

const OSMetaClass *lookupClass(const char *name)
{
    const OSSymbol *symbol = OSSymbol::withCString(name);
    const OSMetaClass *meta = symbol ? OSMetaClass::getMetaClassWithName(symbol) : nullptr;
    OSSafeReleaseNULL(symbol);
    return meta;
}

// IONetworkingFamily loaded, the two original classes the ones this kind's tables describe, and ours registered on
// them. Once: the classes stay registered for the kernel's life.
const char *registerClasses()
{
    if (OSKextLoadKextWithIdentifier("com.apple.iokit.IONetworkingFamily") != kOSReturnSuccess)
        return "com.apple.iokit.IONetworkingFamily did not load (is it prelinked?)";
    const OSMetaClass *controller = lookupClass("IOEthernetController");
    const OSMetaClass *interface = lookupClass("IOEthernetInterface");
    if (controller != &IOEthernetController::gMetaClass || interface != &IOEthernetInterface::gMetaClass ||
        controller->getClassSize() != sizeof(IOEthernetController) ||
        interface->getClassSize() != sizeof(IOEthernetInterface))
        return "the loaded IONetworkingFamily is not the " SWITCH_ABI_KIND " one";
    if (switch_kernel_classes(switch_wlan_family_static_init) != kOSReturnSuccess ||
        lookupClass("IO80211Controller") != &IO80211Controller::gMetaClass ||
        lookupClass("IO80211Interface") != &IO80211Interface::gMetaClass)
        return "registering IO80211Controller and IO80211Interface failed";
    return nullptr;
}

}  // namespace

bool swlan::net::publish(IOService *provider, IOWorkLoop *loop, const uint8_t mac[6])
{
    if (const char *why = registerClasses()) {
        IOLog("SwitchWLAN: no en0: %s\n", why);
        return false;
    }
    gLoop = loop;
    // IONetworkInterface's init reads its controller's provider: "built-in" makes the interface IOBuiltin, and with
    // unit 0 IOPrimaryInterface, which configd's interface namer gives en0.
    if (OSData *builtIn = OSData::withBytes("", 1)) {
        provider->setProperty("built-in", builtIn);
        builtIn->release();
    }
    IO80211Controller *controller = OSTypeAlloc(IO80211Controller);
    if (!controller || !controller->init(nullptr)) {
        OSSafeReleaseNULL(controller);
        IOLog("SwitchWLAN: no en0: no memory for the controller\n");
        return false;
    }
    controller->setMac(mac);
    if (!controller->attach(provider)) {
        controller->release();
        IOLog("SwitchWLAN: no en0: the controller did not attach\n");
        return false;
    }
    if (!controller->start(provider)) {
        controller->detach(provider);
        controller->release();
        IOLog("SwitchWLAN: no en0: IONetworkController::start failed\n");
        return false;
    }
    controller->registerService();
    IONetworkInterface *interface = nullptr;
    if (!controller->attachInterface(&interface, true)) {
        IOLog("SwitchWLAN: no en0: attachInterface failed\n");
        controller->release();
        return false;
    }
    attached(controller, reinterpret_cast<IOEthernetInterface *>(interface));
    controller->release();
    return true;
}
