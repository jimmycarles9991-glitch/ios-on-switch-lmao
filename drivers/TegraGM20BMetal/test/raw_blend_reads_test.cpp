// SPDX-License-Identifier: MIT
// raw_blend_reads.h against Metal's blend equation evaluated per channel: a colour is read where some destination value
// changes the blended result, for every source and destination factor and operation of the colour and of the alpha. The
// rule never misses a read; it counts one only where the equation reads, but for a destination term as the source factor
// against a source term as the destination factor (S x D - D x S, S x (1 - D) + D x S), which cancel to no read.
#include <gtest/gtest.h>

#include <algorithm>

#include "drivers/TegraGM20BMetal/raw/runtime/raw_blend_reads.h"

namespace {

constexpr unsigned kFactors = 19, kOperations = 5;

struct Pixel {
    float color, alpha;
};

constexpr Pixel kSource{0.6f, 0.9f}, kSource1{0.35f, 0.45f}, kConstant{0.15f, 0.55f};
constexpr Pixel kDestinations[] = {{0.25f, 0.5f}, {0.75f, 0.125f}, {0.1f, 0.95f}};

// MTLBlendFactor's value for one channel (the colour's, or the alpha's)
float factor(unsigned f, bool alpha, Pixel d)
{
    const float s = alpha ? kSource.alpha : kSource.color, dc = alpha ? d.alpha : d.color;
    const float s1 = alpha ? kSource1.alpha : kSource1.color, b = alpha ? kConstant.alpha : kConstant.color;
    const float values[kFactors] = {0,
                                    1,
                                    s,
                                    1 - s,
                                    kSource.alpha,
                                    1 - kSource.alpha,
                                    dc,
                                    1 - dc,
                                    d.alpha,
                                    1 - d.alpha,
                                    alpha ? 1 : std::min(kSource.alpha, 1 - d.alpha),
                                    b,
                                    1 - b,
                                    kConstant.alpha,
                                    1 - kConstant.alpha,
                                    s1,
                                    1 - s1,
                                    kSource1.alpha,
                                    1 - kSource1.alpha};
    return values[f];
}

float blend(unsigned src, unsigned dst, unsigned op, bool alpha, Pixel d)
{
    const float s = alpha ? kSource.alpha : kSource.color, dc = alpha ? d.alpha : d.color;
    const float a = s * factor(src, alpha, d), b = dc * factor(dst, alpha, d);
    switch (op) {
    case 0:
        return a + b;
    case 1:
        return a - b;
    case 2:
        return b - a;
    case 3:
        return std::min(s, dc);
    default:
        return std::max(s, dc);
    }
}

bool channelReads(unsigned src, unsigned dst, unsigned op, bool alpha)
{
    const float first = blend(src, dst, op, alpha, kDestinations[0]);
    return std::any_of(std::begin(kDestinations) + 1, std::end(kDestinations), [&](Pixel d) {
        return blend(src, dst, op, alpha, d) != first;
    });
}

// a source factor among DestinationColor..OneMinusDestinationAlpha against SourceColor or SourceAlpha as the destination's
bool cancels(unsigned src, unsigned dst)
{
    return src >= 6 && src <= 9 && (dst == 2 || dst == 4);
}

void expectRule(const raw::ColorKeyFields &c, unsigned src, unsigned dst, unsigned op, bool alpha)
{
    const bool reads = channelReads(src, dst, op, alpha), rule = raw::colorReadsDestination(c);
    EXPECT_TRUE(rule || !reads) << "missed " << src << " " << dst << " " << op;
    EXPECT_TRUE(!rule || reads || cancels(src, dst)) << "extra " << src << " " << dst << " " << op;
}

TEST(RawBlendReads, EveryColourFactorPairAndOperation)
{
    for (unsigned src = 0; src < kFactors; src++)
        for (unsigned dst = 0; dst < kFactors; dst++)
            for (unsigned op = 0; op < kOperations; op++)
                expectRule({80, 15, 1, src, dst, op, 1, 0, 0}, src, dst, op, false);
}

TEST(RawBlendReads, EveryAlphaFactorPairAndOperation)
{
    for (unsigned src = 0; src < kFactors; src++)
        for (unsigned dst = 0; dst < kFactors; dst++)
            for (unsigned op = 0; op < kOperations; op++)
                expectRule({80, 15, 1, 1, 0, 0, src, dst, op}, src, dst, op, true);
}

TEST(RawBlendReads, WriteMaskAndBlendingOff)
{
    EXPECT_FALSE(raw::colorReadsDestination({80, 0, 1, 1, 1, 0, 1, 1, 0}));
    EXPECT_TRUE(raw::colorReadsDestination({80, 1, 0, 1, 0, 0, 1, 0, 0}));
    EXPECT_FALSE(raw::colorReadsDestination({80, 15, 0, 0, 1, 0, 0, 1, 0}));
    EXPECT_FALSE(raw::colorReadsDestination({80, 15, 1, 1, 0, 0, 1, 0, 0}));
    EXPECT_TRUE(raw::colorReadsDestination({80, 15, 1, 0, 1, 0, 0, 1, 0}));
}

}  // namespace
