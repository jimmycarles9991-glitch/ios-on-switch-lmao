// SPDX-License-Identifier: MIT
// pack/resolve_srgb.h's srgbCodeOfLinear, which the sRGB resolve states encode with, against Metal's count of the
// thresholds a 12-bit linear value reaches: for every value, and with the curve's pow moved by far more than a GPU's
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>

namespace {
using uint = uint32_t;
using ushort = uint16_t;

float gPowScale = 1.0f;
}  // namespace

// The Metal standard library functions resolve_srgb.h names, the pow scaled by the test
namespace metal {
float powr(float x, float y)
{
    return std::pow(x, y) * gPowScale;
}

using std::min;
}  // namespace metal

namespace {
#define constant
#include "../pack/resolve_srgb.h"
#undef constant

uint thresholdsReached(uint x)
{
    return uint(std::count_if(std::begin(kSrgbThresholds), std::end(kSrgbThresholds), [x](ushort t) {
        return x >= t;
    }));
}

TEST(SrgbResolveEncode, OneThresholdReadGivesMetalsCodeOfEveryTwelveBitValue)
{
    for (const float scale : {1.0f, 1.0f - 1e-5f, 1.0f + 1e-5f}) {
        gPowScale = scale;
        for (uint x = 0; x < 4096; ++x)
            ASSERT_EQ(srgbCodeOfLinear(x), thresholdsReached(x)) << x << " with pow scaled by " << scale;
    }
    gPowScale = 1.0f;
}

TEST(SrgbResolveEncode, EveryCodesLinearValueEncodesToItsCode)
{
    for (uint code = 0; code < 256; ++code)
        EXPECT_EQ(srgbCodeOfLinear(kSrgbLinear[code]), code);
}
}  // namespace
