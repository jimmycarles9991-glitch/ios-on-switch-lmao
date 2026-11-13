// SPDX-License-Identifier: MIT
// raw/fx/fx_mip_route.h on the host: which engine makes each level of a chain, and the 2D runs' push words through the 2D
// engine model (raw/fx/test/twod_sim), which rounds as referenceDownsample does; the device's own rounding is the HW Test's.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "../raw/fx/fx_image.h"
#include "../raw/fx/fx_mip_route.h"
#include "../raw/fx/test/twod_sim.h"
#include "../raw/runtime/raw_mip.h"

namespace {

using raw::fx::MipChain;
using raw::fx::MipEngine;
using raw::fx::MipRun;

constexpr uint32_t kBgra8 = 0xcf;
constexpr uint64_t kChainVa = 0x2000000000ull;

std::vector<MipRun> runs(const MipChain &c, bool twod, bool threeD, bool *routed = nullptr)
{
    std::vector<MipRun> out;
    const bool ok = raw::fx::eachMipRun(c, twod, threeD, [&](const MipRun &r) {
        out.push_back(r);
    });
    if (routed)
        *routed = ok;
    return out;
}

MipChain chain(uint32_t w, uint32_t h, uint32_t bpp = 4)
{
    MipChain c;
    EXPECT_TRUE(raw::fx::mipChainLayout(w, h, raw::fx::fullLevels(w, h), bpp, c));
    return c;
}

TEST(MipRoute, ProvenStepsAreThoseWhoseWeightsAreQuarters)
{
    for (uint32_t s : {1u, 2u, 3u, 4u, 5u, 6u, 388u, 776u, 1280u})
        EXPECT_TRUE(raw::fx::twodStepProven(s, s > 1 ? s / 2 : 1)) << s;
    for (uint32_t s : {7u, 9u, 11u, 45u, 97u, 255u, 1023u})
        EXPECT_FALSE(raw::fx::twodStepProven(s, s / 2)) << s;
    for (uint32_t s = 7; s < 4096; s += 2) {  // fractions (x + 1/2) s / d - 1/2 against the closed form
        const uint32_t d = s / 2;
        bool quarters = true;
        for (uint32_t x = 0; x < d && quarters; x++)
            quarters = ((2 * x + 1) * s - d) * 2 % d == 0;
        EXPECT_EQ(quarters, raw::fx::twodStepProven(s, d)) << s;
    }
}

// Core Animation's 776x24 chain (backboardd's on every boot): 97 -> 48 is the one step no probe covered
TEST(MipRoute, CoreAnimationStripChain)
{
    const MipChain c = chain(776, 24);
    ASSERT_EQ(c.levels, 10u);
    ASSERT_EQ(c.level[3].width, 97u);

    const std::vector<MipRun> five = runs(c, true, false);
    ASSERT_EQ(five.size(), 1u);
    EXPECT_EQ(five[0].engine, MipEngine::TwoD);
    EXPECT_EQ(five[0].first, 0u);
    EXPECT_EQ(five[0].last, 9u);
    EXPECT_EQ(five[0].unproven, 1u);

    const std::vector<MipRun> srgb = runs(c, true, true);
    ASSERT_EQ(srgb.size(), 3u);
    EXPECT_EQ(srgb[0].engine, MipEngine::TwoD);
    EXPECT_EQ(srgb[0].last, 3u);
    EXPECT_EQ(srgb[1].engine, MipEngine::ThreeD);
    EXPECT_EQ(srgb[1].first, 3u);
    EXPECT_EQ(srgb[1].last, 4u);
    EXPECT_EQ(srgb[2].engine, MipEngine::TwoD);
    EXPECT_EQ(srgb[2].first, 4u);
    EXPECT_EQ(srgb[2].last, 9u);
    for (const MipRun &r : srgb)
        EXPECT_EQ(r.unproven, 0u);

    const std::vector<MipRun> passes = runs(c, false, true);
    ASSERT_EQ(passes.size(), 1u);
    EXPECT_EQ(passes[0].engine, MipEngine::ThreeD);
    EXPECT_EQ(passes[0].last, 9u);

    bool routed = true;
    EXPECT_TRUE(runs(c, false, false, &routed).empty());
    EXPECT_FALSE(routed);
    EXPECT_FALSE(raw::fx::mipRoutable(c, false, false));
}

// Each 2D format's unproven steps have an engine: the pack's mip state where it has one (sRGB), else the 2D engine
TEST(MipRoute, TwoDFormatsAndTheirThreeDStates)
{
    const struct {
        uint32_t mtl, fmt2d;
        bool threeD;
    } formats[] = {{80, 0xcf, false}, {70, 0xd5, false}, {115, 0xca, false}, {10, 0xf3, false},
                   {30, 0xea, false}, {81, 0xd0, true},  {71, 0xd6, true}};

    for (const auto &f : formats) {
        EXPECT_EQ(raw::fx::twodFormat(f.mtl), f.fmt2d) << f.mtl;
        EXPECT_EQ(raw::mipState(f.mtl) != nullptr, f.threeD) << f.mtl;
    }
    for (uint32_t mtl : {1u, 20u, 25u, 40u, 43u, 55u, 60u, 65u, 90u, 92u, 94u, 105u, 110u, 125u}) {
        EXPECT_EQ(raw::fx::twodFormat(mtl), 0u) << mtl;
        EXPECT_NE(raw::mipState(mtl), nullptr) << mtl;
    }
}

TEST(MipRoute, PowerOfTwoChainsAreOneTwoDRun)
{
    for (const auto &[w, h] : {std::pair{1024u, 1024u}, {128u, 128u}, {1280u, 720u}}) {
        const MipChain c = chain(w, h);
        const std::vector<MipRun> r = runs(c, true, true);
        if (w == 1280) {  // 1280x720 reaches 45 -> 22 and 5x2 -> 2x1 (5 -> 2 proven)
            EXPECT_GT(r.size(), 1u);
            continue;
        }
        ASSERT_EQ(r.size(), 1u) << w;
        EXPECT_EQ(r[0].engine, MipEngine::TwoD);
        EXPECT_EQ(r[0].last, c.levels - 1);
    }
}

// Texel byte (xB, y) of level `l` of layer `layer`
uint8_t &texel(std::vector<uint8_t> &mem, const MipChain &c, uint32_t layer, uint32_t l, uint32_t xB, uint32_t y)
{
    return mem[size_t(layer * c.arrayStride + raw::fx::texelOffset(c, l, xB, y))];
}

void fill(std::vector<uint8_t> &mem, const MipChain &c, uint32_t layer, uint32_t l, uint32_t seed)
{
    for (uint32_t y = 0; y < c.level[l].height; y++)
        for (uint32_t xB = 0; xB < c.level[l].width * c.bpp; xB++)
            texel(mem, c, layer, l, xB, y) = uint8_t((xB * 37 + y * 101 + seed * 53) ^ (xB >> 2));
}

std::vector<uint8_t> level(const std::vector<uint8_t> &mem, const MipChain &c, uint32_t layer, uint32_t l)
{
    std::vector<uint8_t> out(size_t(c.level[l].width) * c.level[l].height * c.bpp);
    for (uint32_t y = 0; y < c.level[l].height; y++)
        for (uint32_t xB = 0; xB < c.level[l].width * c.bpp; xB++)
            out[size_t(y) * c.level[l].width * c.bpp + xB] =
                mem[size_t(layer * c.arrayStride + raw::fx::texelOffset(c, l, xB, y))];
    return out;
}

// emitGenerateLayers of levels first + 1 .. last through the model: each such level is the reference step from the level above
// as the model left it, the levels outside the run keep their bytes, and the run is ordered and ends sampleable
void checkRun(const MipChain &c, uint32_t first, uint32_t last)
{
    std::vector<uint8_t> mem(c.bytes * c.layers, 0);
    for (uint32_t layer = 0; layer < c.layers; layer++)
        for (uint32_t l = 0; l < c.levels; l++)
            fill(mem, c, layer, l, layer * 16 + l);
    const std::vector<uint8_t> before = mem;
    std::vector<uint32_t> push(raw::fx::twodInitWords() + raw::fx::generateLayersWords(c, first, last));
    raw::Push p{push.data(), push.data() + push.size()};
    raw::fx::emitTwodInit(p);
    raw::fx::emitGenerateLayers(p, c, kChainVa, kBgra8, first, last);
    ASSERT_LE(p.at, push.data() + push.size());
    twodsim::Sim sim;
    sim.map(kChainVa, mem);
    ASSERT_TRUE(sim.run(push.data(), size_t(p.at - push.data()))) << sim.error();
    EXPECT_TRUE(sim.endsSampleable);
    EXPECT_EQ(sim.launches, (last - first) * c.layers);
    for (uint32_t layer = 0; layer < c.layers; layer++)
        for (uint32_t l = 0; l < c.levels; l++) {
            const std::vector<uint8_t> got = level(mem, c, layer, l);
            if (l <= first || l > last) {
                EXPECT_EQ(got, level(before, c, layer, l))
                    << "layer " << layer << " level " << l << " outside the run changed";
                continue;
            }
            const std::vector<uint8_t> above = level(mem, c, layer, l - 1);
            std::vector<uint8_t> want(got.size());
            raw::fx::referenceDownsample(above.data(), c.level[l - 1].width * c.bpp, c.level[l - 1].width,
                                         c.level[l - 1].height, want.data(), c.level[l].width * c.bpp, c.level[l].width,
                                         c.level[l].height, c.bpp);
            EXPECT_EQ(got, want) << "layer " << layer << " level " << l;
        }
}

TEST(MipRoute, TwoDRunsWriteOnlyTheirLevels)
{
    const MipChain c = chain(20, 12);
    checkRun(c, 0, c.levels - 1);
    checkRun(c, 1, 3);
    checkRun(c, 2, 3);
}

TEST(MipRoute, TwoDRunsAroundAThreeDRun)
{
    const MipChain c = chain(776, 24);
    checkRun(c, 0, 3);
    checkRun(c, 4, 9);
}

TEST(MipRoute, EveryLayerOfAnArrayChain)
{
    MipChain c;
    ASSERT_TRUE(raw::fx::arrayLayout(16, 8, 3, 5, 4, c));
    ASSERT_EQ(c.layers, 3u);
    checkRun(c, 0, 4);
    checkRun(c, 1, 2);
}

}  // namespace
