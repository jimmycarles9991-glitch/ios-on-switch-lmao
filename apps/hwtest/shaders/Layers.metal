#include <metal_stdlib>
using namespace metal;

// metal.native.layered_vertex and metal.native.viewport_index: instance i of a quad goes to layer i, or through
// viewport i, in colour colors[i]

struct FlatColor {
    float4 color [[user(color), flat]];
};

struct LayeredVertex {
    float4 position [[position]];
    uint layer [[render_target_array_index]];
    float4 color [[user(color), flat]];
};

vertex LayeredVertex layered_vertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                    constant float4 *corners [[buffer(0)]], constant float4 *colors [[buffer(1)]])
{
    return LayeredVertex{corners[vid], iid, colors[iid]};
}

struct ViewportVertex {
    float4 position [[position]];
    uint viewport [[viewport_array_index]];
    float4 color [[user(color), flat]];
};

vertex ViewportVertex viewport_vertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                      constant float4 *corners [[buffer(0)]], constant float4 *colors [[buffer(1)]])
{
    return ViewportVertex{corners[vid], iid, colors[iid]};
}

fragment float4 flat_color_fragment(FlatColor in [[stage_in]])
{
    return in.color;
}
