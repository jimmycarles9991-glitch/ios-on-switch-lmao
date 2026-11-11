#include <metal_stdlib>
using namespace metal;

kernel void multiply_add(device const float *a [[buffer(0)]], device const float *b [[buffer(1)]],
                         device float *out [[buffer(2)]], device atomic_uint *counter [[buffer(3)]],
                         uint id [[thread_position_in_grid]])
{
    out[id] = fma(a[id], b[id], float(id & 1023));
    atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
}

kernel void fma_loop(device float4 *data [[buffer(0)]], constant uint &iterations [[buffer(1)]],
                     uint id [[thread_position_in_grid]])
{
    float4 x = data[id];
    float4 y = x * 0.5f + 1.0f;
    for (uint i = 0; i < iterations; ++i) {
        x = fma(x, y, 0.75f);
        y = fma(y, x, -0.25f);
    }
    data[id] = x + y;
}

struct Vertex {
    float4 position [[position]];
};

/// Four corners per quad in clip space, z included, drawn as a triangle strip.
vertex Vertex quad_vertex(uint vid [[vertex_id]], constant float4 *corners [[buffer(0)]])
{
    return Vertex{corners[vid]};
}

fragment float4 solid_fragment(constant float4 &color [[buffer(0)]])
{
    return color;
}

/// Writes exactly the floats it is given, so the render target's own float-to-format conversion is what is tested.
fragment float4 exact_fragment(constant float4 &value [[buffer(0)]])
{
    return value;
}

kernel void read_uint_view(texture2d<uint, access::read> view [[texture(0)]], device uint *out [[buffer(0)]],
                           uint2 id [[thread_position_in_grid]])
{
    out[id.y * view.get_width() + id.x] = view.read(id).r;
}

kernel void read_float_texture(texture2d<float, access::read> texture [[texture(0)]],
                               device float4 *out [[buffer(0)]], uint2 id [[thread_position_in_grid]])
{
    if (id.x < texture.get_width() && id.y < texture.get_height())
        out[id.y * texture.get_width() + id.x] = texture.read(id);
}

/// Samples at texel centres with nearest filtering: the only way to read a block-compressed texture back.
kernel void sample_texture(texture2d<float> texture [[texture(0)]], device float4 *out [[buffer(0)]],
                           uint2 id [[thread_position_in_grid]])
{
    constexpr sampler nearest(filter::nearest);
    const float2 size = float2(texture.get_width(), texture.get_height());
    if (id.x < texture.get_width() && id.y < texture.get_height())
        out[id.y * texture.get_width() + id.x] = texture.sample(nearest, (float2(id) + 0.5f) / size, level(0));
}

/// m2v writes a half-float texture through its unsigned integer twin, converting each texel in the shader: the raw path
/// binds a header of that format over the texture's memory for these (gpu.raw.kernel_view).
kernel void view_half_write(texture2d<half> source [[texture(0)]], texture2d<half, access::write> rgba [[texture(1)]],
                            texture2d<half, access::write> red [[texture(2)]], uint2 id [[thread_position_in_grid]])
{
    constexpr sampler nearest(filter::nearest);
    const float2 size = float2(source.get_width(), source.get_height());
    const half4 s = source.sample(nearest, (float2(id) + 0.5f) / size, level(0));
    rgba.write(s * 2.0h + half4(half(id.x), half(id.y), 0.25h, -1.0h), id);
    red.write(half4(half(id.x + id.y) * 0.5h), id);
}

kernel void view_bgra_write(texture2d<float, access::write> bgra [[texture(0)]], uint2 id [[thread_position_in_grid]])
{
    bgra.write(float4(id.x, id.y, id.x + id.y, 255 - id.x) / 255.0f, id);
}

kernel void view_half_read_write(texture2d<half, access::read_write> texture [[texture(0)]],
                                 uint2 id [[thread_position_in_grid]])
{
    texture.write(texture.read(id) * 0.5h + half4(0.25h, -0.5h, half(id.x), 1.0h), id);
}
