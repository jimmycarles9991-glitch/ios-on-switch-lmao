// SPDX-License-Identifier: MIT
// The viewports and scissor rectangles a range replayed on m2v hands its render encoder. m2v's encoder implements
// setViewport: and setScissorRect: alone (NativeBridgeMetalFrontend.mm); setViewports:count: and setScissorRects:count:
// it answers zero, so a replay sent through them draws without a scissor or viewport. A raster state of at most one each
// replays through the single forms; one of more is a draw m2v has no form of (RawMetalM2vGuard.mm).
#pragma once
#include <cstdint>

namespace raw {

// A raster state's count as the plan keeps it: 0 where the encoder set none (Metal's default, one)
constexpr bool m2vSingleView(uint32_t count)
{
    return count <= 1;
}

constexpr const char *m2vViewsMissing(uint32_t viewports, uint32_t scissors)
{
    if (!m2vSingleView(viewports))
        return "more than one viewport";
    if (!m2vSingleView(scissors))
        return "more than one scissor rectangle";
    return nullptr;
}

}  // namespace raw
