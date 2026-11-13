// SPDX-License-Identifier: MIT
// Stored multisampled textures and a pass's sample layout, as NVK writes them: a multisampled image is NIL's image of its
// sample grid (fx::msLayout) under a header of its MULTI_SAMPLE_COUNT, which texture2d_ms reads and the resolve state samples;
// a pass of more than one sample sets SET_ANTI_ALIAS, the sixteen SET_ANTI_ALIAS_SAMPLE_POSITIONS locations and the root
// table's sample words (nvk_flush_ms_state), at the standard positions or the pass's own.
#pragma once
#include <cstdint>
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "../fx/fx_image.h"
#include "raw_push.h"
#include "raw_sample_positions.h"
#include "raw_samples.h"

namespace raw {

// The header of a `samples`-sample image of `bpp`-byte texels at `va`, header word 0 `tic0`; false for one sample or a sample
// count NIL has no layout for
inline bool multisampleHeader(uint32_t width, uint32_t height, uint32_t samples, uint32_t bpp, uint64_t va,
                              uint32_t tic0, bool srgb, uint32_t out[8])
{
    fx::MipChain c;
    if (samples < 2 || !fx::msLayout(width, height, samples, bpp, c))
        return false;
    fx::mipChainTic(c, va, tic0, srgb, 0, 1, out);
    return true;
}

inline bool sampleLayoutAt(uint32_t samples, const SamplePositions &at, SampleLayout &out)
{
    if (!sampleLayout(samples, out))
        return false;
    placeSamplePositions(at, samples, out.positions, out.root);
    return true;
}

// The layout's methods; the root table (`rootBytes` at `rootVa`) is left selected
inline void sampleLayoutMethods(Push &p, const SampleLayout &l, uint32_t rootBytes, uint64_t rootVa)
{
    p.m(NVB197_SET_ANTI_ALIAS, l.antiAlias);
    p.incArray(NVB197_SET_ANTI_ALIAS_SAMPLE_POSITIONS(0), l.positions, 4);
    p.inc(NVB197_SET_CONSTANT_BUFFER_SELECTOR_A, {rootBytes, uint32_t(rootVa >> 32), uint32_t(rootVa)});
    p.m(NVB197_LOAD_CONSTANT_BUFFER_OFFSET, kRootSampleInfo);
    p.nonInc(NVB197_LOAD_CONSTANT_BUFFER(0), l.root, kRootSampleInfoWords);
}

}  // namespace raw
