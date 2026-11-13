// SPDX-License-Identifier: MIT
// Rule fill.constant: a solid fill drawn by the ROP's constant colour (SET_CONSTANT_COLOR_RENDERING,
// kits/SwitchGM20BEmit/SwitchGM20BEmitRaster.h) with the pixel program's output unused. Core Animation's fill states take
// their colour from the vertex stream, so a draw qualifies only when every vertex it reads carries one colour, which the
// CPU reads from the stream at the draw. The unit is undocumented: false until HW Test gpu.x1.constant_colour's float arm
// reads the constant exactly (its covered texels at the constant, the rest at the clear) and its timing shows the saving.
#pragma once
#include <cstdint>

namespace raw {

constexpr bool ropConstantColour()
{
    return false;
}

struct FillDraw {
    bool fillState = false;       // a state whose only colour input is the vertex stream's (Pipeline::fillState)
    bool samplesTexture = false;  // its record binds a texture
    bool shapesCoverage = false;  // discard, a sample mask output or depth written by the program
    bool oneTarget = false;       // colour 0 alone is bound
    bool oneColour = false;       // every vertex the draw reads has the same colour
};

constexpr bool constantColourFill(const FillDraw &d)
{
    return ropConstantColour() && d.fillState && !d.samplesTexture && !d.shapesCoverage && d.oneTarget && d.oneColour;
}

}  // namespace raw
