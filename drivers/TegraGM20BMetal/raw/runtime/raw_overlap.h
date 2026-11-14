// SPDX-License-Identifier: MIT
// A framebuffer-fetching draw that writes a color it fetches through the texture unit must see, at each pixel, what its
// own earlier triangles wrote there (Metal's raster order). The fetch barrier orders draws, not the triangles of one
// draw, so such a draw is drawn one triangle a run, with the barrier between runs. A draw in its state's storage form
// orders its fragments inside the pixel interlock and is drawn whole.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "raw_fetch_order.h"

namespace raw {

class TriangleRuns {
public:
    static constexpr uint32_t kMaxTriangles = kSequentialTriangles;

    // A list or strip needs no CPU geometry to preserve raster order one triangle at a time; instanced draws need a
    // separate instance-order plan, so they are not admitted here. An indexed draw's runs are index ranges. Runs retain
    // the original vertex IDs and provoking vertex; restarting an odd strip triangle reverses winding. An indexed strip's
    // run holding its restart index draws nothing under the hardware's primitive restart, as the whole strip draws no
    // triangle there; past a restart at an even index its runs' faces are the opposite of Metal's, which the caller
    // admits only where faces decide no pixel (FetchDraw::facing; a fragment program's front_facing is no bit of its
    // header, NAK reads it as attribute 0x3fc).
    static bool sequential(uint32_t primitive, uint32_t count, uint32_t instances,
                           std::vector<std::pair<uint32_t, uint32_t>> &runs, std::array<bool, kMaxTriangles> &winding)
    {
        runs.clear();
        winding.fill(false);
        const bool strip = primitive == 4;
        if (instances != 1 || (primitive != 3 && !strip))
            return false;
        const uint32_t triangles = strip ? (count > 2 ? count - 2 : 0) : count / 3;
        if (triangles > kMaxTriangles)
            return false;
        for (uint32_t i = 0; i < triangles; ++i) {
            const uint32_t first = strip ? i : i * 3;
            runs.emplace_back(first, first + 3);
            winding[i] = strip && (i & 1);
        }
        return true;
    }
};

// Whether the face a triangle shows decides its pixels: culling (MTLCullMode), or a stencil test on an attachment with
// stencil whose front and back faces differ in their state or reference. Both faces passing always and keeping their
// value test nothing (RenderBox's icon draws: Depth32Float_Stencil8 attached, depth tested, stencil untouched).
template <typename DepthStencilDesc>
bool facingDecides(uint8_t cull, const DepthStencilDesc &ds, const uint32_t references[2], bool stencilAttached)
{
    if (cull)
        return true;
    if (!stencilAttached)
        return false;
    const auto &f = ds.front, &b = ds.back;
    const auto untouched = [](const auto &face) {
        return face.compare == 7 && face.depthFail == 0 && face.pass == 0;
    };
    if (untouched(f) && untouched(b))
        return false;
    const bool sameFaces = f.compare == b.compare && f.fail == b.fail && f.depthFail == b.depthFail &&
                           f.pass == b.pass && f.readMask == b.readMask && f.writeMask == b.writeMask &&
                           std::min(references[0], 0xffu) == std::min(references[1], 0xffu);
    return !sameFaces;
}

}  // namespace raw
