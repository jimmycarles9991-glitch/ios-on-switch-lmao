#include <metal_stdlib>
using namespace metal;

// tests/StitchedLibraryTest.swift: the visible functions it stitches into stitch_madd(a, b) = (a + b) * b
[[stitchable]] float stitch_add(float a, float b) {
    return a + b;
}

[[stitchable]] float stitch_mul(float a, float b) {
    return a * b;
}

// tests/StitchedLibraryTest.swift: specialized with stitch_gain, stitched into stitch_scaled and linked into
// stitch_consumer, which writes out[i] = i * gain + 3
constant uint stitch_gain [[function_constant(3)]];

[[stitchable]] float stitch_scaled_add(float a, float b) {
    return a * float(stitch_gain) + b;
}

[[visible]] float stitch_scaled(float a, float b);

kernel void stitch_consumer(device float *out [[buffer(0)]], uint tid [[thread_position_in_grid]]) {
    out[tid] = stitch_scaled(float(tid), 3.0f);
}
