// SPDX-License-Identifier: MIT
// SET_ANTI_ALIAS_ALPHA_CONTROL as a Metal state asks it (MTLRenderPipelineDescriptor alphaToCoverageEnabled and
// alphaToOneEnabled), written by the encoder as NVK's nvk_flush_ms_state writes it from the pipeline's dynamic state. A
// state's key (RawPipelineKey.h) holds neither, so states that differ only in them share one record and one
// raw::Pipeline: the encoder writes the register from the Metal state each draw binds, after the record's registers. A
// state whose own registers set it for itself (a reach twin) keeps its value.
#pragma once
#include <cstdint>

#include "raw_blend_kill.h"

namespace raw {

constexpr uint32_t kAlphaControlMethod = 0x153c;  // NVB197_SET_ANTI_ALIAS_ALPHA_CONTROL
constexpr uint32_t kAlphaToCoverage = 1u << 0, kAlphaToOne = 1u << 4;

constexpr uint32_t alphaControl(bool toCoverage, bool toOne)
{
    return (toCoverage ? kAlphaToCoverage : 0) | (toOne ? kAlphaToOne : 0);
}

constexpr uint32_t kAlphaControlUnknown = ~0u;

// The words before a draw, `held` what the GPU holds (kAlphaControlUnknown: not known) and `wanted` the bound Metal
// state's: `rebound` the state's registers were written for this draw, `names` they set the register, `owns` for the
// state's own purpose; `blendKill` they drop zero-source pixels (raw_blend_kill.h), which alpha to one would make wrong,
// as it gives a zero source an alpha of one
template <typename Push>
void writeAlphaControl(Push &p, uint32_t &held, uint32_t wanted, bool rebound, bool names, bool owns, bool blendKill)
{
    if (rebound && names)
        held = kAlphaControlUnknown;
    if (owns)
        return;
    const bool changed = wanted != held;
    if (changed) {
        p.m(kAlphaControlMethod, wanted);
        held = wanted;
    }
    if ((wanted & kAlphaToOne) && blendKill && (rebound || changed))
        p.m(kIteratedBlendOptimization, kNoopNever);
}

}  // namespace raw
