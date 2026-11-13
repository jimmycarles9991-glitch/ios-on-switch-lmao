#include <metal_stdlib>
using namespace metal;

/// gpu.raw.integer_targets: the words it is given, into an unsigned integer target
fragment uint4 integer_uint_fragment(constant uint4 &value [[buffer(0)]])
{
    return value;
}

/// The same into a signed integer target
fragment int4 integer_sint_fragment(constant int4 &value [[buffer(0)]])
{
    return value;
}
