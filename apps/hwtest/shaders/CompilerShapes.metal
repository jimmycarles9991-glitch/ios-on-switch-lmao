#include <metal_stdlib>
using namespace metal;

/// gpu.gm20b.sm53_layout: nested if/else and a loop with continue and break, each pixel's branches its own; red the
/// if/else path, green the loop's sum, blue the pixel's index (SM53Layout.path is the same code on the CPU)
fragment float4 sm53_layout_paths(float4 position [[position]])
{
    const uint x = uint(position.x), y = uint(position.y), a = x * 7 + y * 13;
    uint path;
    if ((a & 3) == 0) {
        path = 1;
        if (x & 4) {
            path += 10;
        } else {
            path += 20;
            if (y & 2)
                path += 100;
        }
    } else if ((a & 3) == 1) {
        path = 2;
    } else {
        path = 3;
        if (x > y)
            path += 30;
        else if (x == y)
            path += 40;
        else
            path += 50 + (y & 1);
    }
    uint sum = 0;
    for (uint i = 0; i <= (x & 15); i++) {
        if ((i + y) % 5 == 0)
            continue;
        sum += i * (y % 9 + 1);
        if (sum > 120 + (x & 63))
            break;
        if ((i ^ x) & 8)
            sum ^= 0x55;
        else
            sum += 3;
    }
    return float4(float(path), float(sum), float(y * 256 + x), 1);
}

/// gpu.gm20b.float_tests: each value's classes and comparisons as bits of red (FloatClasses.bits on the CPU); `shape` is
/// the target's width and the values' count
fragment float4 float_classes(float4 position [[position]], device const float *values [[buffer(0)]],
                              constant uint2 &shape [[buffer(1)]])
{
    const uint i = uint(position.y) * shape.x + uint(position.x);
    if (i >= shape.y)
        return float4(0);
    const float v = values[i], m = fabs(v);
    uint bits = uint(isnan(v)) | uint(isinf(v)) << 1 | uint(isfinite(v)) << 2 | uint(isnormal(v)) << 3;
    bits |= uint(m > 0.0f) << 4 | uint(m < FLT_MIN) << 5 | uint(m >= FLT_MIN) << 6 | uint(m < 1.0f) << 7;
    bits |= uint(m <= FLT_MAX) << 8 | uint(m == INFINITY) << 9 | uint(m != INFINITY) << 10 | uint(v < 0.0f) << 11;
    bits |= uint(v > 0.0f) << 12 | uint(v == 0.0f) << 13 | uint(v != v) << 14 | uint(signbit(v)) << 15;
    return float4(float(bits), 0, 0, 1);
}

/// gpu.gm20b.powr_specials: powr of each half pair (pairs[i].x, pairs[i].y) as red; `shape` is the target's width and
/// the pairs' count
fragment float4 powr_specials(float4 position [[position]], device const half2 *pairs [[buffer(0)]],
                              constant uint2 &shape [[buffer(1)]])
{
    const uint i = uint(position.y) * shape.x + uint(position.x);
    if (i >= shape.y)
        return float4(0);
    return float4(float(powr(pairs[i].x, pairs[i].y)), 0, 0, 1);
}

/// gpu.gm20b.half_min_max: fmax and fmin of each half pair (pairs[i].x, pairs[i].y) as red and green, the clamp of x to
/// [y, 1] as blue, and as alpha abs(x) < y plus two for x < 0 (a transfer curve's tests); `shape` is the target's width
/// and the pairs' count
fragment float4 half_min_max(float4 position [[position]], device const half2 *pairs [[buffer(0)]],
                             constant uint2 &shape [[buffer(1)]])
{
    const uint i = uint(position.y) * shape.x + uint(position.x);
    if (i >= shape.y)
        return float4(0);
    const half x = pairs[i].x, y = pairs[i].y;
    const float tests = float(abs(x) < y) + 2 * float(x < half(0));
    return float4(float(fmax(x, y)), float(fmin(x, y)), float(fmin(fmax(x, y), half(1))), tests);
}
