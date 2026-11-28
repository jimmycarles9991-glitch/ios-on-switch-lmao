#include <metal_stdlib>
using namespace metal;

struct BytesAddressVertex {
    float4 position [[position]];
    float4 colour;
};

/// gpu.gm20b.bytes_address: two setVertexBytes bindings compared by address, as Weather's VFX compares buffer 5's, so the
/// translation reads each slot's Metal address (m2v SlotGuestBase). Two bindings are two addresses: the colour is left's
vertex BytesAddressVertex bytes_address_vertex(uint vid [[vertex_id]], constant float4 *corners [[buffer(0)]],
                                               constant float4 *left [[buffer(5)]], constant float4 *right [[buffer(6)]])
{
    return {corners[vid], left == right ? float4(0) : left[0]};
}

fragment float4 bytes_address_fragment(BytesAddressVertex in [[stage_in]])
{
    return in.colour;
}
