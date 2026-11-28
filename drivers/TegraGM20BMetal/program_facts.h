// SPDX-License-Identifier: MIT
// A program's PackProgramFact bits (pack.h) from its shader header (SPHv3, cla097sph.h), and the facts Device::build reads
// back from them
#pragma once
#include <cstdint>
#include <cstring>

#include "pack.h"
#include "viewports.h"

namespace gm20b {

constexpr size_t kProgramHeaderBytes = 0x50;

inline uint32_t headerFacts(const uint8_t *sph)
{
    uint32_t w[20];
    memcpy(w, sph, sizeof w);
    uint32_t facts = 0;
    facts |= (w[0] >> 16 & 1) ? kProgramGlobalStore : 0;
    facts |= headerWritesViewportIndex(sph) ? kProgramWritesViewportIndex : 0;
    facts |= headerWritesLayer(sph) ? kProgramWritesLayer : 0;
    facts |= (w[5] >> 24 & 1) ? kProgramPrimitiveId : 0;  // bit 184
    facts |= (w[0] >> 15 & 1) ? kProgramKillsPixels : 0;
    facts |= (w[19] >> 1 & 1) ? kProgramWritesDepth : 0;  // bit 609
    facts |= (w[19] & 1) ? kProgramWritesSampleMask : 0;  // bit 608
    return facts;
}

constexpr uint32_t kHeaderFacts = kProgramGlobalStore | kProgramWritesViewportIndex | kProgramWritesLayer |
                                  kProgramPrimitiveId | kProgramKillsPixels | kProgramWritesDepth |
                                  kProgramWritesSampleMask;

}  // namespace gm20b
