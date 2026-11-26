// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchXHCI's IOKit plumbing that needs nothing of AppleUSBXHCI's layout: the host stack's resource personalities and
// the provider nub.
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

}  // namespace xhost
