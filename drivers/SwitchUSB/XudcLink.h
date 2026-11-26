// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

namespace xudc {
enum : int {
    kSpeedNone = -1,
    kSpeedFull = 1,
    kSpeedHigh = 2,
    kSpeedSuper = 3
};

// USB 3.2 9.6.2.3: full/high/SuperSpeed, full-speed minimum, conservative maximum exit latencies.
constexpr uint8_t kSuperSpeedCapability[] = {10, 16, 3, 0, 0x0e, 0, 1, 10, 0xff, 7};

constexpr int portSpeed(uint32_t portsc)
{
    switch ((portsc >> 10) & 0xfu) {
    case 1:
        return kSpeedFull;
    case 3:
        return kSpeedHigh;
    case 4:
        return kSpeedSuper;
    default:
        return kSpeedNone;
    }
}

constexpr uint32_t ep0Packet(int speed)
{
    return speed == kSpeedSuper ? 512u : 64u;
}

constexpr uint32_t endpointBurst(int speed, int type, uint32_t maxPacket, uint32_t superSpeedBulkMaxBurst)
{
    if (speed == kSpeedSuper)
        return type == 2 ? superSpeedBulkMaxBurst : 0u;
    if (speed == kSpeedHigh && (type == 1 || type == 3)) {
        const uint32_t extra = (maxPacket >> 11) & 3u;
        return extra > 2 ? 2u : extra;
    }
    return 0;
}
}  // namespace xudc
