// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <IOKit/IONVRAM.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include "../../kits/SwitchDT/SwitchDTProperty.h"

inline bool switch_platform_native_nvram_ready(IOService *provider)
{
    IORegistryEntry *options = provider->childFromPath("options", gIODTPlane);
    const auto *nvram = OSDynamicCast(IODTNVRAM, options);
    bool ready = false;
    if (nvram && !nvram->isInactive() && nvram->getProvider() == provider) {
        OSIterator *children = nvram->getChildIterator(gIOServicePlane);
        if (children) {
            while (auto *entry = children->getNextObject()) {
                const auto *service = OSDynamicCast(IOService, entry);
                if (!service || !service->metaCast("IODTNVRAMDiags") || service->isInactive() ||
                    service->getProvider() != nvram)
                    continue;
                OSObject *property = service->copyProperty("Version");
                const auto *version = OSDynamicCast(OSNumber, property);
                ready = version && version->unsigned32BitValue() != 0;
                if (property)
                    property->release();
                if (ready)
                    break;
            }
            children->release();
        }
    }
    if (options)
        options->release();
    return ready;
}

inline bool switch_platform_nvram_ready(IOService *provider)
{
    IORegistryEntry *chosen = provider->childFromPath("chosen", gIODTPlane);
    if (!chosen)
        return false;
    const bool bank = switch_dt_length(chosen, "nvram-bank-size") == sizeof(uint32_t);
    const bool source = chosen->getProperty("nvram-proxy-data") != nullptr;
    const bool valid = switch_dt_length(chosen, "nvram-proxy-data") != 0;
    chosen->release();
    // RELEASE XNU consumes the proxy before platform matching; Version records the parsed native format.
    return bank && (source ? valid : switch_platform_native_nvram_ready(provider));
}
