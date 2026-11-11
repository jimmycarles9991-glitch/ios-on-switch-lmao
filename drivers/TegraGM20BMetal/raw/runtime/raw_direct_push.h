// SPDX-License-Identifier: MIT
#pragma once
#include "bridge/NativeMetalDirect.h"

namespace raw {

// The bind-offset draw changes every buffer residual; only words shared by all probe draws can be folded.
inline uint32_t constantPushWords(const NmbDirectState &state)
{
    uint32_t mask = state.draws[0].pushMask;
    for (const NmbDirectDraw &d : state.draws)
        for (uint32_t i = 0; i < 32; i++)
            if (!(d.pushMask >> i & 1) || d.push[i] != state.draws[0].push[i])
                mask &= ~(1u << i);
    return mask;
}

}  // namespace raw
