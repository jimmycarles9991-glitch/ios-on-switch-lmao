// SPDX-License-Identifier: MIT
// A pass's own sample positions (MTLRenderPassDescriptor setSamplePositions:count:) as the hardware and NAK take them: the
// SET_ANTI_ALIAS_SAMPLE_POSITIONS words and the root table's draw.sample_locations (nvk_flush_ms_state, a grid of one pixel),
// each coordinate where Metal samples it (metal2vulkan sample_positions.rs, measured on an M4).
#pragma once
#include <cstdint>

namespace raw {

// x | y << 4 in sixteenths of a pixel a sample; count 0: the standard positions
struct SamplePositions {
    uint8_t count = 0;
    uint8_t xy[8] = {};
};

// The nearest sixteenth, a tie up, at most 15/16
inline uint8_t positionSixteenths(float v)
{
    const float s = v * 16.0f + 0.5f;
    return s >= 15.0f ? 15 : uint8_t(int32_t(s));
}

// `count` (x, y) pairs for a pass of `samples` samples; false where Metal refuses them (a count other than 2 or 4, more
// positions than samples, a coordinate outside [0, 1)). Samples past the positions sit at the pixel's corner.
inline bool takeSamplePositions(const float *xy, uint32_t count, uint32_t samples, SamplePositions &out)
{
    out = SamplePositions();
    if ((count != 2 && count != 4) || (samples != 2 && samples != 4) || count > samples)
        return false;
    for (uint32_t i = 0; i < 2 * count; i++)
        if (!(xy[i] >= 0.0f && xy[i] < 1.0f))
            return false;
    for (uint32_t s = 0; s < count; s++)
        out.xy[s] = uint8_t(positionSixteenths(xy[2 * s]) | positionSixteenths(xy[2 * s + 1]) << 4);
    out.count = uint8_t(count);
    return true;
}

// The `count` positions a pass holds (getSamplePositions:nil count:0), read by `read(xy, count)`. Metal's
// getSamplePositions:count: asserts unless a non-zero count is exactly the number the pass holds, so exactly `count` are
// read, and none past eight.
template <typename Read>
inline bool readSamplePositions(uint32_t count, uint32_t samples, Read &&read, SamplePositions &out)
{
    out = SamplePositions();
    if (!count || count > 8)
        return false;
    float xy[16] = {};
    read(xy, count);
    return takeSamplePositions(xy, count, samples, out);
}

// `p` over a layout of `samples` samples: the sixteen hardware locations (the pixel's, repeated over the 2x2 quad) and the
// root table's first two words (eight locations); nothing for the standard positions
inline void placeSamplePositions(const SamplePositions &p, uint32_t samples, uint32_t hardware[4], uint32_t root[2])
{
    if (!p.count || !samples || samples > 8)
        return;
    for (uint32_t w = 0; w < 4; w++) {
        hardware[w] = 0;
        for (uint32_t k = 0; k < 4; k++)
            hardware[w] |= uint32_t(p.xy[(4 * w + k) % samples]) << (8 * k);
    }
    root[0] = root[1] = 0;
    for (uint32_t s = 0; s < samples; s++)
        root[s / 4] |= uint32_t(p.xy[s]) << (8 * (s % 4));
}

}  // namespace raw
