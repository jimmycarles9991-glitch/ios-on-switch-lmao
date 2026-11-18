#include <metal_stdlib>
using namespace metal;

struct StencilOut {
    float4 colour [[color(0)]];
    uint stencil [[stencil]];
};

/// The colour of buffer(0) and, as both faces' stencil reference, the pixel's column plus buffer(1)'s offset (the bits
/// past 8 dropped, as Metal takes the output's low 8 bits)
fragment StencilOut shadow_stencil_fragment(float4 p [[position]], constant float4 &colour [[buffer(0)]],
                                            constant uint &offset [[buffer(1)]])
{
    return {colour, uint(p.x) + offset};
}
