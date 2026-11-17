// SPDX-License-Identifier: MIT
// A state's colour attachment rendered into a stand-in of another format (raw_ycbcr_store.h) blends as the attachment's
// format would. A format that holds no alpha reads its destination alpha as 1: an M4 Max blending a two-plane YCbCr
// attachment by DestinationAlpha stores 0 codes off a destination alpha of 1 and all but none off the alpha its clear held.
// Such an attachment's factors of the destination alpha are the constants they come to, so the stand-in's alpha is never read.
#pragma once
#include <cstdint>

namespace raw {

// MTLBlendFactor
constexpr uint32_t kBlendZero = 0, kBlendOne = 1, kBlendDestinationAlpha = 8, kBlendOneMinusDestinationAlpha = 9,
                   kBlendSourceAlphaSaturated = 10;

// `factor` of a colour (`rgb`) or alpha blend where the destination alpha is 1: SourceAlphaSaturated is min(As, 1 - Ad) for
// a colour and 1 for alpha (MTLRenderPipeline.h)
constexpr uint32_t alphaOneFactor(uint32_t factor, bool rgb)
{
    switch (factor) {
    case kBlendDestinationAlpha:
        return kBlendOne;
    case kBlendOneMinusDestinationAlpha:
        return kBlendZero;
    case kBlendSourceAlphaSaturated:
        return rgb ? kBlendZero : kBlendOne;
    default:
        return factor;
    }
}

}  // namespace raw
