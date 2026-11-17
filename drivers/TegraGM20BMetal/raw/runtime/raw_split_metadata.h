// SPDX-License-Identifier: MIT
#pragma once
#include "raw_cbuf.h"
#include "raw_pack.h"

namespace raw {

// A split's frontier is bound as one constant buffer, whole 256-byte units of at most kCbufMaxBytes (the chip honours a
// bind's size to the byte up to SET_CONSTANT_BUFFER_SELECTOR_A SIZE's 64 KiB), and PackSplit::frontierBytes is 16 bits:
// its largest 16-byte multiple. nvk_switch_split.h and pack/splits.py bound the frontiers they make and pack the same.
constexpr uint32_t kMaxFrontierBytes = 0xFFF0;
static_assert(kMaxFrontierBytes == kCbufMaxBytes - 16 && cbufBindBytes(kMaxFrontierBytes) == kCbufMaxBytes);

inline bool usableSplitMetadata(const PackSplit &split)
{
    const SplitCost &cost = split.cost;
    if (cost.version > 1 || cost.reserved ||
        (cost.version == 1 && (split.batchVersion != 1 || !cost.launchCycles || !cost.drainCycles ||
                               !cost.prologueWarpCycles || !cost.prologueInstructions || !cost.savedPixelInstructions ||
                               !cost.frontierValues || cost.frontierValues > split.frontierBytes / 4)))
        return false;
    if (!split.frontierBytes || split.frontierBytes > kMaxFrontierBytes || (split.frontierBytes & 15) ||
        !split.frontierCbuf || split.frontierCbuf >= 16 || split.uboCount > 6)
        return false;
    uint16_t used = 0;
    for (unsigned i = 0; i < split.uboCount; ++i) {
        const unsigned slot = split.uboCbuf[i];
        if (slot >= split.frontierCbuf || (used & (1u << slot)))
            return false;
        used |= 1u << slot;
    }
    if (split.qmd[19] != 0x10001)
        return false;
    switch (split.batchVersion) {
    case 0:
        return !split.batchThreads && (split.qmd[18] >> 16) == 1 &&
               (!split.shaderDataCbuf || split.shaderDataCbuf == 1 + split.uboCount) &&
               split.prologueCbufs == 1 + split.uboCount + (split.shaderDataCbuf != 0);
    case 1:
        // UBO ordinals address record views, never QMD CB1's shared shader constants.
        return split.batchThreads && split.batchThreads <= 1024 && (split.qmd[18] >> 16) == split.batchThreads &&
               split.shaderDataCbuf <= 1 && split.prologueCbufs == 1 + split.shaderDataCbuf;
    default:
        return false;
    }
}

}  // namespace raw
