// SPDX-License-Identifier: MIT
// What a draw's facts select among a stage's program variants (raw_pack.h PackVariant, raw_variants.cpp)
#pragma once
#include <cstdint>
#include "raw_pack.h"

namespace raw {

// MTLPixelFormat values whose texels are unorm, snorm or sRGB codes: a sample of one is a number in [-1, 1], never a NaN
inline bool fixedPointTexels(uint32_t format)
{
    switch (format) {
    case 1:    // A8Unorm
    case 10:   // R8Unorm
    case 11:   // R8Unorm_sRGB
    case 12:   // R8Snorm
    case 20:   // R16Unorm
    case 22:   // R16Snorm
    case 30:   // RG8Unorm
    case 31:   // RG8Unorm_sRGB
    case 32:   // RG8Snorm
    case 40:   // B5G6R5Unorm
    case 41:   // A1BGR5Unorm
    case 42:   // ABGR4Unorm
    case 43:   // BGR5A1Unorm
    case 60:   // RG16Unorm
    case 62:   // RG16Snorm
    case 70:   // RGBA8Unorm
    case 71:   // RGBA8Unorm_sRGB
    case 72:   // RGBA8Snorm
    case 80:   // BGRA8Unorm
    case 81:   // BGRA8Unorm_sRGB
    case 90:   // RGB10A2Unorm
    case 94:   // BGR10A2Unorm
    case 110:  // RGBA16Unorm
    case 112:  // RGBA16Snorm
        return true;
    default:
        return false;
    }
}

// Whether `rec` is one of `pack`'s own records. A record a process made (raw_completed.cpp) is not, though Device::pipeline
// answers its key with it: its state's variants are made as it was, never looked up in the pack.
inline bool packHolds(const Pack &pack, const PackPipeline *rec)
{
    const PackPipeline *first = pack.section<PackPipeline>(PackPipelines);
    return rec >= first && rec < first + pack.header()->pipelines;
}

// The first of `count` variants whose assumptions `facts` hold, `count` (the generic program) where none do
inline uint32_t chooseVariant(const PackVariant *variants, uint32_t count, uint32_t facts)
{
    for (uint32_t i = 0; i < count; i++)
        if (!(variants[i].assume & ~facts))
            return i;
    return count;
}

}  // namespace raw
