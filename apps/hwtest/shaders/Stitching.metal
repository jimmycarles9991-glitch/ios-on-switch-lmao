#include <metal_stdlib>
using namespace metal;

// tests/StitchedLibraryTest.swift: the visible functions it stitches into stitch_madd(a, b) = (a + b) * b
[[stitchable]] float stitch_add(float a, float b) {
    return a + b;
}

[[stitchable]] float stitch_mul(float a, float b) {
    return a * b;
}
