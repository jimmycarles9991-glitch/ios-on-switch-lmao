// SPDX-License-Identifier: MIT
// A fresh texture's first contents (raw_fresh.h), written by the copy engine: every level of a chain, level 0 the texture
// itself and each later one chainLevelImage's, every layer or depth plane of each (a volume's levels have planes of their
// own), the first `patterned` levels with `pattern`'s texel and the rest with `zero`'s.
#pragma once
#include "raw_fresh.h"
#include "raw_gpu.h"

namespace raw {

// The bytes it wrote
inline uint64_t fillFresh(BlitEncoder &b, const Texture &x, uint32_t patterned, const CopyFill &pattern,
                          const CopyFill &zero)
{
    const uint32_t levels = x.chain ? x.chain->layout.levels : 1;
    uint64_t bytes = 0;
    for (uint32_t level = 0; level < levels; level++) {
        Texture image = x;
        if (level && !chainLevelImage(x, level, image))
            continue;
        uint32_t sx, sy;
        sampleGrid(image.samples, sx, sy);
        image.width <<= sx;
        image.height <<= sy;
        image.samples = 1;
        const uint32_t planes = image.type == fx::kTexThreeD ? image.depth : image.layers;
        for (uint32_t z = 0; z < planes; z++)
            b.fillTexels(image, z, image.width, image.height, level < patterned ? pattern : zero);
        bytes += uint64_t(image.width) * image.height * planes * x.bpp;
    }
    return bytes;
}

}  // namespace raw
