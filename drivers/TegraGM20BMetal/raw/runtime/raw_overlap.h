// SPDX-License-Identifier: MIT
// A framebuffer-fetching draw that writes a color it fetches through the texture unit must see, at each pixel, what its
// own earlier triangles wrote there (Metal's raster order). The fetch barrier orders draws, not the triangles of one
// draw, so such a draw is drawn one triangle a run, with the barrier between runs. A draw in its state's storage form
// orders its fragments inside the pixel interlock and is drawn whole.
#pragma once
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "raw_fetch_order.h"

namespace raw {

class TriangleRuns {
public:
    static constexpr uint32_t kMaxTriangles = kSequentialTriangles;

    // A list or strip needs no CPU geometry to preserve raster order one triangle at a time. An indexed strip needs
    // its restart positions, and instanced draws need a separate instance-order plan, so neither is admitted here.
    // An indexed list's runs are index ranges. Runs retain the original vertex IDs and provoking vertex; restarting
    // an odd strip triangle reverses winding.
    static bool sequential(uint32_t primitive, uint32_t count, uint32_t instances, bool indexed,
                           std::vector<std::pair<uint32_t, uint32_t>> &runs, std::array<bool, kMaxTriangles> &winding)
    {
        runs.clear();
        winding.fill(false);
        const bool strip = primitive == 4;
        if ((indexed && strip) || instances != 1 || (primitive != 3 && !strip))
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

}  // namespace raw
