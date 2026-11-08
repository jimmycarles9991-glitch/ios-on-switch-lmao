// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchXHCI's IOKit plumbing that needs nothing of AppleUSBXHCI's layout: the host stack's resource personalities, the
// provider nub, and the USB-C connector's root port.
#pragma once
#include <IOKit/IOReturn.h>
#include <stddef.h>
#include <stdint.h>

class IOService;
class OSMetaClass;

namespace xhost {

const OSMetaClass *lookupClass(const char *name);

// An IOResources personality, as in the kext's Info.plist, into the catalogue (matched now).
bool addResourcePersonality(const char *ioClass, const char *category, const char *bundle,
                            const char *resource = "IOKit");

// Provider nub: register window index 0 and `intid` as interrupt index 0 (the GIC nub's own interrupt properties, so
// IOService resolves it through ArmGIC). Retained; nullptr on failure.
IOService *makeNub(uint64_t registers, uint64_t bytes, uint32_t intid);

// AppleUSBXHCI's root port for xHCI port `number` once it is registered (its setProperties takes nothing before):
// the AppleUSBHostPort under `controller` whose "port" (kUSBHostPortPropertyPortNumber, AppleUSBXHCIPort's 4-byte
// OSData) is that number. Waits up to `timeoutNS` (thread context). Retained; nullptr if none registered in time.
IOService *waitForRootPort(IOService *controller, unsigned number, uint64_t timeoutNS);

// One line of the controller's children: "<class> port <n> power <state>" each (n from "port", ? without one).
void describePorts(const IOService *controller, char *buffer, size_t size);

// UsbForcePower (IOUSBHostFamilyDefinitions.h kUSBHostPropertyForcePower) on an AppleUSBHostController or an
// AppleUSBHostPort runs its own forcePower and waits for the state (the port: 1 s, the controller: 6 s). On a port it is
// the one its cableChangeOccurred and the removable and Type-C port classes call: On holds the port On until a device
// attaches (then the device's needs set the power), off holds it Off. On the controller On is a raise: the controller
// powers up (its registers become AppleUSBXHCI's to read) and then floats with its ports' needs. Thread context.
IOReturn forcePower(IOService *service, bool on);

}  // namespace xhost
