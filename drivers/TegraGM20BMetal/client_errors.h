// SPDX-License-Identifier: MIT
// What a draw or copy that Metal's validation layer would stop does on the driver: what Apple's driver does with it, its
// command buffer completed with no error. Measured on an M-series Mac with API validation off (x1-m2v-errors' probes):
//  - a state of RGBA8Unorm drawn into a BGRA8Unorm attachment writes the right colour (the target orders the channels);
//    one of RGBA16Float into BGRA8Unorm writes the half-float output's bits, which this GPU's ROP has no form of: skipped
//  - a state of one sample in a pass of four writes sample 0 alone (a full-target triangle resolves to a quarter of its
//    colour); a state of four in a pass of one draws the whole colour
//  - indirect draw arguments past their buffer read the bytes after it (zeros here, from no memory but the push itself)
//  - a copy or fill past a buffer moves the bytes inside both buffers and leaves the rest; one that starts past moves none
#pragma once
#include <algorithm>
#include <cstdint>

namespace gm20b {

// MTLPixelFormat values of the four-channel 8-bit unorm targets, which differ in channel order alone
inline bool targetOrdersChannels(uint32_t state, uint32_t pass)
{
    const auto plain = [](uint32_t f) {
        return f == 70 || f == 80;  // RGBA8Unorm, BGRA8Unorm
    };
    const auto srgb = [](uint32_t f) {
        return f == 71 || f == 81;  // RGBA8Unorm_sRGB, BGRA8Unorm_sRGB
    };
    return (plain(state) && plain(pass)) || (srgb(state) && srgb(pass));
}

// Of a state's sample count and its pass's, whether the draw is drawn (with a sample mask of sampleMaskFor's)
inline bool samplesDrawn(uint32_t stateSamples, uint32_t passSamples, uint32_t statePasses)
{
    if (stateSamples == passSamples)
        return true;
    return statePasses == 1 && (stateSamples == 1 || passSamples == 1);
}

// SET_SAMPLE_MASK_X0_Y0..X1_Y1's word for every pixel of a quad: sample 0 alone for a one-sample state in a pass of more
constexpr uint32_t kSampleMaskUnknown = ~0u, kSampleMaskAll = 0xffff, kSampleMaskFirst = 1;

inline uint32_t sampleMaskFor(uint32_t stateSamples, uint32_t passSamples)
{
    return stateSamples == 1 && passSamples > 1 ? kSampleMaskFirst : kSampleMaskAll;
}

// The words a copy of `size` bytes from `from` into `to` moves inside buffers of `fromLength` and `toLength` bytes
inline uint64_t clampedCopy(uint64_t fromLength, uint64_t from, uint64_t toLength, uint64_t to, uint64_t size)
{
    if (from >= fromLength || to >= toLength)
        return 0;
    return std::min({size, fromLength - from, toLength - to});
}

// The words of `words` indirect arguments at `offset` of a buffer of `length` bytes that the buffer holds (the rest
// are zeros)
inline uint32_t argumentWordsInside(uint64_t length, uint64_t offset, uint32_t words)
{
    return offset >= length ? 0 : uint32_t(std::min<uint64_t>(words, (length - offset) / 4));
}

}  // namespace gm20b
