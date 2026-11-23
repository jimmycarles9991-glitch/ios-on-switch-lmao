// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

// A chroma location code from IOSurface's bulk attachments: 1 Left, 2 Center, 3 TopLeft, 4 Top, 5 BottomLeft, 6 Bottom
// (0: none given); a code above 6 is no siting VIC has.
static inline uint32_t switch_surface_chroma_code(uint32_t code)
{
    return code <= 6 ? code : UINT32_MAX;
}

static inline int switch_surface_chroma_site(uint32_t top, uint32_t bottom, uint32_t *site)
{
    if (top > 6 || bottom > 6 || (top && bottom && top != bottom))
        return 0;
    *site = top ? top : bottom;
    return 1;
}

// A DC window's colour stage is its YCbCr matrix on co-sited (Left) chroma. The window has no transfer or gamut stage
// and VIC uses its own (TegraVICColor.h) only for IOSurfaceAccelerator transforms, so a surface's transfer and
// primaries never decide which of them scans it. A siting VIC cannot convert either (fields that differ, a code above
// 6) is scanned co-sited: VIC's conversion would refuse it and the layer would go unshown.
static inline int switch_surface_dc_siting(uint32_t top, uint32_t bottom)
{
    uint32_t chroma = 0;
    return !switch_surface_chroma_site(top, bottom, &chroma) || chroma <= 1;
}
