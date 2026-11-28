#include <metal_stdlib>
using namespace metal;

// A rasterization rate map's data held by an argument buffer, as Weather's VFX particle functions hold theirs (a
// pointer member of no stated size), and bound directly; beside it a pointer to a struct the library only declares, as
// WeatherUI's functions hold VFX's light buffer

struct RateMapDeclared;

struct RateMapHeld {
    float scale [[id(0)]];
    constant rasterization_rate_map_data *map [[id(1)]];
    constant RateMapDeclared *declared [[id(2)]];
};

vertex float4 rate_map_held_vertex(uint v [[vertex_id]], constant RateMapHeld &held [[buffer(2)]])
{
    return float4(float(v & 1) * held.scale, float(v >> 1), 0, 1);
}

vertex float4 rate_map_direct_vertex(uint v [[vertex_id]], constant rasterization_rate_map_data &map [[buffer(3)]],
                                     constant float &scale [[buffer(4)]], constant RateMapDeclared *declared [[buffer(5)]])
{
    return float4(float(v & 1) * scale, float(v >> 1), 0, 1);
}

fragment half4 rate_map_fragment()
{
    return half4(1);
}
