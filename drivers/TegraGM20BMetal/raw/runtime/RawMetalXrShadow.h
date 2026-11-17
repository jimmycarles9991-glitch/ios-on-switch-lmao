// SPDX-License-Identifier: MIT
// The command buffer's extended-range shadows (raw_xr_shadow.h): the open render encoder's draws as they are recorded, and each
// ended pass that holds shadows with the shadowed variant of every state its draws take there.
#pragma once
#include <cstdint>
#include <vector>

#include "raw_cubic.h"
#include "raw_variant.h"
#include "raw_written_textures.h"
#include "raw_xr_shadow.h"

@class RMPipeline;

namespace raw {
struct Pipeline;
}

// A state the open pass draws that reads or writes one of its eligible colours, as its draws record it
struct RMXrShadowDraw {
    __unsafe_unretained RMPipeline *pipe;
    raw::Variant<raw::Pipeline> *base;  // the variant its draws record (null: the state's own record)
    raw::CubicBinds binds;
    uint8_t tests;
    bool planes;
    uint16_t xrSlots;
    std::vector<raw::WrittenTexture> written;
    uint8_t touched;  // the eligible colours it reads or writes
};

struct RMXrShadowOpen {
    bool open = false;
    uint32_t passAt = 0;
    size_t renderAt = 0;
    raw::XrShadowPass pass;
    std::vector<RMXrShadowDraw> draws;
};

struct RMXrShadowTwin {
    const void *base;  // its draws' variant, or its RMPipeline where they record none
    raw::Variant<raw::Pipeline> *shadowed;
};

// A pass that holds shadows: its OpRender in the plan, its RMPass, and the shadowed variant of each state it draws
struct RMXrShadowTwins {
    size_t renderAt = 0;
    uint32_t passAt = 0;
    std::vector<RMXrShadowTwin> twins;
};

// The pipeline a draw of `variant`'s state (or of `pipe`'s own record) takes in a pass of `twins`: its shadowed variant's,
// else `drawn`
inline raw::Pipeline *rmXrShadowed(const RMXrShadowTwins *twins, const void *pipe, raw::Variant<raw::Pipeline> *variant,
                                   raw::Pipeline *drawn)
{
    if (!twins)
        return drawn;
    const void *base = variant ? static_cast<const void *>(variant) : pipe;
    for (const RMXrShadowTwin &t : twins->twins)
        if (t.base == base)
            return t.shadowed->value();
    return drawn;
}
