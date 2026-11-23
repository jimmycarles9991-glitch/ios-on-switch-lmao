// SPDX-License-Identifier: MIT
// test/linkcheck's callee: the visible function caller.metal's linked_frag links.
#include <metal_stdlib>
using namespace metal;

[[visible]] float4 custom_fn(float4 color, float scale)
{
    return float4(color.bgr * scale, color.a);
}
