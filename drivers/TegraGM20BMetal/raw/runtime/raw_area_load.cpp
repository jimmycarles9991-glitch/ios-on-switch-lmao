// SPDX-License-Identifier: MIT
// See raw_area_load.h.
#include "raw_area_load.h"

#include "raw_gpu.h"
#include "raw_reach.h"

namespace raw {

bool loadPastArea(CommandBuffer &rc, Texture &t, uint32_t areaW, uint32_t areaH, const PassReach &reach, uint32_t tag,
                  const char **why)
{
    const Rotation own = frameOf(t);
    const PlaneOffset o = areaLoadOffset(own, areaW, areaH);
    if ((!o.x && !o.y) || !reach.mask)
        return true;
    Texture *before = rc.turnScratch(turnCopyFormat(t.bpp), t.width, t.height, tag);
    if (!before) {
        *why = "no scratch for a turned attachment loaded past the render area";
        return false;
    }
    BlitEncoder *b = rc.blit();
    b->copyTexels(t, 0, 0, 0, *before, 0, 0, 0, t.width, t.height);
    b->end();
    const TurnMask mask = turnMask(reach, frameToPlane(own));
    AreaLoadCopy copies[4];
    for (uint32_t i = 0, n = areaLoadCopies(o, copies); i < n; i++)
        if (!rc.turnCopy(t, *before, copies[i].map, why, &mask, copies[i].width, copies[i].height))
            return false;
    return true;
}

}  // namespace raw
