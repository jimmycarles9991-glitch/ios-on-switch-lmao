// SPDX-License-Identifier: MIT
// R10Unorm_PACKED and RG10Unorm_PACKED of several levels over a surface plane (Texture::levelZero): Metal reads level 0 at
// every lod, a read at lod L level-0 texel (x, y), while the shader sees each level's size (an M4 Max:
// m2v-textures11 probes/h4/mips.txt). The image is a chain of as many levels, whose header answers the levels and their sizes,
// and each level L is level 0's top-left (W >> L) x (H >> L), which the copy engine copies once the image is drawn.
// About half of an M4 Max's dispatches over such a texture read some threadgroups' codes a multiple of 128 bytes further
// on, a different offset each run: a race in Apple's driver, which these copies do not mirror.
#pragma once
#include "gpu.h"

namespace gm20b {

// Whether a sampler header that reads level 0 alone (sampler.h levelZeroHeader) samples `t` as its own header does:
// such a texture, or one of one level, which every lod reads anyway (MIP_POINT with LOD clamps [0, 1/4] keeps a
// minification as one). A view holds no chain whatever its levels: taken as mipmapped
inline bool samplesLevelZeroAlone(const Texture &t)
{
    return t.levelZero || (!t.view && (!t.chain || t.chain->layout.levels <= 1));
}

// Levels 1 and on of `t` from its level 0, on the copy engine
inline void copyLevelZero(BlitEncoder &e, const Texture &t)
{
    if (!t.levelZero || !t.chain)
        return;
    for (uint32_t level = 1; level < t.chain->layout.levels; level++) {
        Texture image;
        if (chainLevelImage(t, level, image))
            e.copyTexels(t, 0, 0, 0, image, 0, 0, 0, image.width, image.height);
    }
}

}  // namespace gm20b
