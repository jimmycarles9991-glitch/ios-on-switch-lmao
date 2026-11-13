// SPDX-License-Identifier: MIT
#pragma once
#include "raw_pack.h"

namespace raw {

inline bool usableSplitMetadata(const PackSplit &split)
{
    const SplitCost &cost = split.cost;
    if (cost.version > 1 || cost.reserved ||
        (cost.version == 1 && (split.batchVersion != 1 || !cost.launchCycles || !cost.drainCycles ||
                               !cost.prologueWarpCycles || !cost.prologueInstructions || !cost.savedPixelInstructions ||
                               !cost.frontierValues || cost.frontierValues > split.frontierBytes / 4)))
        return false;
    if (!split.frontierBytes || split.frontierBytes > 4096 || (split.frontierBytes & 15) || !split.frontierCbuf ||
        split.frontierCbuf >= 16 || split.uboCount > 6)
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
