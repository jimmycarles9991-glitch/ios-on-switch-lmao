#include <metal_stdlib>
using namespace metal;

struct R5MaskDepth {
    float depth [[depth(any)]];
};

/// gpu.gm20b.mixed_samples: sample s of a pixel at depth 1 where bit s of the mask is set, else 0; a mask of 16 or more
/// takes each pixel's own from a hash of its coordinates (MixedSamplesCases.hashedMask)
fragment R5MaskDepth r5_mask_depth_fragment(float4 position [[position]], uint sample [[sample_id]],
                                            constant uint &mask [[buffer(0)]])
{
    const uint2 px = uint2(position.xy);
    uint h = (px.x * 0x9E3779B1u) ^ (px.y * 0x85EBCA77u);
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    const uint m = mask < 16 ? mask : (h & 15);
    return {((m >> sample) & 1) ? 1.0f : 0.0f};
}

struct R5LayerVertex {
    float4 position [[position]];
    uint layer [[render_target_array_index]];
    float4 colour [[flat]];
};

/// Layer `first` + instance, in the layer's colour ((layer + 1) / 5, 0.25, 0.75, 1): no channel's unorm8 code is a tie
vertex R5LayerVertex r5_layer_vertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                     constant float4 *corners [[buffer(0)]], constant uint &first [[buffer(1)]])
{
    const uint layer = first + iid;
    return {corners[vid], layer, float4(float(layer + 1) / 5.0f, 0.25f, 0.75f, 1.0f)};
}

fragment float4 r5_layer_colour(R5LayerVertex in [[stage_in]])
{
    return in.colour;
}

struct R5LayerColours {
    float4 c0 [[color(0)]];
    float4 c1 [[color(1)]];
};

fragment R5LayerColours r5_layer_colours(R5LayerVertex in [[stage_in]])
{
    return {in.colour, in.colour};
}

fragment float4 r5_layer_fetch(R5LayerVertex in [[stage_in]], float4 previous [[color(0)]])
{
    return float4(previous.rgb + in.colour.rgb, 1);
}

kernel void r5_read_texel_buffer(texture_buffer<uint, access::read> texels [[texture(0)]],
                                 constant uint *at [[buffer(0)]], device uint *out [[buffer(1)]],
                                 uint i [[thread_position_in_grid]])
{
    out[i] = texels.read(at[i]).r;
}
