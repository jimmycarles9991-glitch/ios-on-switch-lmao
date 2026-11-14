#include <metal_stdlib>
using namespace metal;

/// gpu.raw.mip_lod: a mipmapped texture sampled through the bound sampler at the level of detail its coordinates'
/// derivatives give (textured_vertex's uv), with no bias or with the shader's bias from buffer 0
struct MipLodVertex {
    float4 position [[position]];
    float2 uv;
};

fragment float4 mip_lod_fragment(MipLodVertex in [[stage_in]], texture2d<float> texture [[texture(0)]],
                                 sampler bound [[sampler(0)]])
{
    return texture.sample(bound, in.uv);
}

fragment float4 mip_lod_bias_fragment(MipLodVertex in [[stage_in]], texture2d<float> texture [[texture(0)]],
                                      sampler bound [[sampler(0)]], constant float &lodBias [[buffer(0)]])
{
    return texture.sample(bound, in.uv, bias(lodBias));
}
