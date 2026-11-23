#include <metal_stdlib>
using namespace metal;

// Spelled out, not a loop, so the program holds every FMA and its words scale with the count
#define FRAME_WEIGHTS_FMA8 \
    x = fma(x, y, 0.25f);  \
    x = fma(x, y, 0.25f);  \
    x = fma(x, y, 0.25f);  \
    x = fma(x, y, 0.25f);  \
    x = fma(x, y, 0.25f);  \
    x = fma(x, y, 0.25f);  \
    x = fma(x, y, 0.25f);  \
    x = fma(x, y, 0.25f);
#define FRAME_WEIGHTS_FMA64 \
    FRAME_WEIGHTS_FMA8 FRAME_WEIGHTS_FMA8 FRAME_WEIGHTS_FMA8 FRAME_WEIGHTS_FMA8 FRAME_WEIGHTS_FMA8 FRAME_WEIGHTS_FMA8 \
        FRAME_WEIGHTS_FMA8 FRAME_WEIGHTS_FMA8
#define FRAME_WEIGHTS_FMA512 \
    FRAME_WEIGHTS_FMA64 FRAME_WEIGHTS_FMA64 FRAME_WEIGHTS_FMA64 FRAME_WEIGHTS_FMA64 FRAME_WEIGHTS_FMA64 \
        FRAME_WEIGHTS_FMA64 FRAME_WEIGHTS_FMA64 FRAME_WEIGHTS_FMA64

/// gpu.diag.frame_weights' B: 64 dependent FMAs on the position, written out
fragment float4 frame_weights_fma64(float4 position [[position]])
{
    float x = position.x * 0x1p-11f, y = position.y * 0x1p-10f;
    FRAME_WEIGHTS_FMA64
    return float4(x, y, 0.0f, 1.0f);
}

/// C: 512 of them
fragment float4 frame_weights_fma512(float4 position [[position]])
{
    float x = position.x * 0x1p-11f, y = position.y * 0x1p-10f;
    FRAME_WEIGHTS_FMA512
    return float4(x, y, 0.0f, 1.0f);
}
