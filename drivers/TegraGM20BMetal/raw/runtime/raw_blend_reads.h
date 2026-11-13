// SPDX-License-Identifier: MIT
// Whether a state's draws read a colour attachment as it was before them, from the colour's fields in the state key
// (RawPipelineKey.h: "c<i>=<format>:<writeMask>:<blending>:<src RGB>:<dst RGB>:<RGB op>:<src A>:<dst A>:<A op>"): it is
// written through a mask that keeps channels, or blended so that the result depends on the destination. A destination
// factor other than Zero, a source factor of the destination (DestinationColor, OneMinusDestinationColor,
// DestinationAlpha, OneMinusDestinationAlpha, and for colour SourceAlphaSaturated, min(As, 1 - Ad); for alpha it is 1) or a
// Min or Max operation, which ignore the factors, reads it; Zero destination factors under Add, Subtract or
// ReverseSubtract write the source terms alone.
#pragma once

namespace raw {

struct ColorKeyFields {
    unsigned format, writeMask, blending, srcRGB, dstRGB, rgbOp, srcAlpha, dstAlpha, alphaOp;
};

// MTLBlendFactorDestinationColor through MTLBlendFactorOneMinusDestinationAlpha, and as a colour factor
// MTLBlendFactorSourceAlphaSaturated
inline bool factorReadsDestination(unsigned factor, bool alpha)
{
    return factor >= 6 && factor <= (alpha ? 9u : 10u);
}

// MTLBlendOperationMin, MTLBlendOperationMax
inline bool operationReadsDestination(unsigned op)
{
    return op == 3 || op == 4;
}

inline bool blendReadsDestination(const ColorKeyFields &c)
{
    return c.dstRGB != 0 || c.dstAlpha != 0 || factorReadsDestination(c.srcRGB, false) ||
           factorReadsDestination(c.srcAlpha, true) || operationReadsDestination(c.rgbOp) ||
           operationReadsDestination(c.alphaOp);
}

inline bool colorReadsDestination(const ColorKeyFields &c)
{
    return c.writeMask && (c.writeMask != 15 || (c.blending && blendReadsDestination(c)));
}

}  // namespace raw
