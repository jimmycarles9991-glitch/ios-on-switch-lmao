// SPDX-License-Identifier: MIT
// Copyright © 2025 Valve Corporation (the reference regions: Mesa src/nouveau/nil/zcull.rs tests, dumped from NVIDIA's
// driver)
// raw/runtime/raw_zcull.h on the host: a region's subregions are nil_zcull_new's (its reference table, for a desktop GPU's
// layout), its words fit kZcullRegionWords, and a pass's draws invalidate it exactly where zcull cannot follow them.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../raw/runtime/raw_zcull.h"

namespace {

constexpr TegraGM20BAbiZcullInfo kReference = {224, 32, 3584, 5120, 128, 224, 1344, 16, 224, 64, 657408, 4096};

struct Row {
    uint32_t width, height, aliquots, normalized, algorithm, perSubregion, format;
};

TEST(Zcull, RegionsAreNilsReferenceOnes)
{
    // (width, height, aliquot count, normalized aliquots, adaptive, a static row's aliquots and format per subregion)
    const Row rows[] = {
        {224, 32, 64, 16, 0, 4, 8},          {448, 256, 128, 32, 0, 8, 8},        {896, 512, 512, 128, 0, 32, 8},
        {1792, 1024, 2048, 512, 0, 128, 8},  {3584, 1024, 4096, 1024, 0, 256, 8}, {448, 16384, 5120, 2048, 1, 0, 0},
        {7168, 16384, 5120, 32768, 1, 0, 0},
    };
    for (const Row &r : rows) {
        raw::ZcullRegion region;
        ASSERT_TRUE(raw::zcullRegion(kReference, 0, 0, r.width, r.height, region)) << r.width << "x" << r.height;
        EXPECT_EQ(region.aliquotCount, r.aliquots) << r.width << "x" << r.height;
        EXPECT_EQ(region.normalizedAliquots, r.normalized) << r.width << "x" << r.height;
        EXPECT_EQ(region.algorithm, r.algorithm) << r.width << "x" << r.height;
        EXPECT_EQ(region.subregionCount, 16u);
        if (r.algorithm)
            continue;
        for (uint32_t i = 0; i < 16; i++)
            EXPECT_EQ(region.subregions[i], i | r.perSubregion << 8 | r.format << 24) << r.width << "x" << i;
    }
}

// The X1's layout (TegraGM20BZcull.h over its registers: one GPC of two TPCs, four zcull banks, 2048 aliquots) over its
// screen and smaller passes: aligned to the subregions' alignment, within the zcull RAM. Its 1280x720 takes every
// subregion: fifteen at 8x8 (Z_8X8_4X2, 120 aliquots) and the first at 4x8 (Z_4X8_2X2, 240), 2040 of 2048.
TEST(Zcull, RegionsCoverTheRenderAreaAligned)
{
    const TegraGM20BAbiZcullInfo x1 = {32, 32, 1024, 2048, 32, 32, 192, 16, 32, 64, 0x1500, 0x1000};
    raw::ZcullRegion screen;
    ASSERT_TRUE(raw::zcullRegion(x1, 0, 0, 1280, 720, screen));
    EXPECT_EQ(screen.algorithm, 1u);
    EXPECT_EQ(screen.aliquotCount, 2040u);
    EXPECT_EQ(screen.subregions[0], 240u << 8 | 8u << 24);
    for (uint32_t i = 1; i < 16; i++)
        EXPECT_EQ(screen.subregions[i], i | 120u << 8 | 5u << 24) << i;
    for (const auto &[w, h] : {std::pair{1280u, 720u}, {720u, 1280u}, {33u, 65u}, {1u, 1u}}) {
        raw::ZcullRegion region;
        ASSERT_TRUE(raw::zcullRegion(x1, 0, 0, w, h, region)) << w << "x" << h;
        EXPECT_GE(region.width, w);
        EXPECT_GE(region.height, h);
        EXPECT_EQ(region.width % 32, 0u);
        EXPECT_EQ(region.height % 64, 0u);
        EXPECT_LE(region.aliquotCount, x1.aliquot_total);
    }
    raw::ZcullRegion none;
    EXPECT_FALSE(raw::zcullRegion(x1, 0, 0, 16384, 16384, none));  // no subregion of it fits the RAM
    TegraGM20BAbiZcullInfo empty = x1;
    empty.aliquot_total = 0;
    EXPECT_FALSE(raw::zcullRegion(empty, 0, 0, 64, 64, none));
    empty = x1;
    empty.subregion_count = 17;
    EXPECT_FALSE(raw::zcullRegion(empty, 0, 0, 64, 64, none));
}

TEST(Zcull, RegionWordsFit)
{
    raw::ZcullRegion region;
    ASSERT_TRUE(raw::zcullRegion(kReference, 0, 0, 1280, 720, region));
    std::vector<uint32_t> words(raw::kZcullRegionWords + 8);
    raw::Push p{words.data(), words.data() + words.size()};
    raw::zcullRegionWords(p, region, 252, raw::kZcullGreater);
    EXPECT_LE(uint32_t(p.at - words.data()), raw::kZcullRegionWords);
}

raw::DepthStencilDesc depth(uint8_t compare, bool write)
{
    raw::DepthStencilDesc d;
    d.depthCompare = compare;
    d.depthWrite = write;
    return d;
}

constexpr uint8_t kNever = 0, kLess = 1, kEqual = 2, kLessEqual = 3, kGreater = 4, kNotEqual = 5, kGreaterEqual = 6,
                  kAlways = 7;

TEST(Zcull, TrackableDepthStatesKeepTheRegion)
{
    raw::ZcullPass z;
    z.candidate = true;
    for (auto [compare, write] : {std::pair{kLess, true},
                                  {kLessEqual, true},
                                  {kEqual, true},
                                  {kLess, false},
                                  {kAlways, false},
                                  {kNever, true},
                                  {kEqual, false}})
        EXPECT_FALSE(raw::zcullInvalidates(z, depth(compare, write), true, false, false, false)) << int(compare);
    EXPECT_EQ(z.dir, raw::kZcullLess);
    raw::ZcullPass reverse;
    reverse.candidate = true;
    EXPECT_FALSE(raw::zcullInvalidates(reverse, depth(kGreaterEqual, true), true, false, false, false));
    EXPECT_EQ(reverse.dir, raw::kZcullGreater);
}

TEST(Zcull, UntrackableStatesInvalidateOnce)
{
    const auto invalidates = [](const raw::DepthStencilDesc &d, bool stencil, bool fragmentDepth, bool shaderTests) {
        raw::ZcullPass z;
        z.candidate = true;
        raw::zcullInvalidates(z, depth(kLess, true), true, stencil, false, false);
        const bool first = raw::zcullInvalidates(z, d, true, stencil, fragmentDepth, shaderTests);
        const bool again = raw::zcullInvalidates(z, d, true, stencil, fragmentDepth, shaderTests);
        return first && !again;
    };
    EXPECT_TRUE(invalidates(depth(kGreater, false), false, false, false));  // the direction flips
    EXPECT_TRUE(invalidates(depth(kGreaterEqual, true), false, false, false));
    EXPECT_TRUE(invalidates(depth(kAlways, true), false, false, false));
    EXPECT_TRUE(invalidates(depth(kNotEqual, true), false, false, false));
    EXPECT_TRUE(invalidates(depth(kLess, true), false, true, false));  // the program writes depth
    EXPECT_TRUE(invalidates(depth(kLess, true), false, false, true));  // the shader's tests
    raw::DepthStencilDesc zfail = depth(kLess, false);
    zfail.back.depthFail = 6;  // IncrementWrap: shadow volumes
    EXPECT_TRUE(invalidates(zfail, true, false, false));
    raw::DepthStencilDesc fail = depth(kLess, true);
    fail.front.compare = 3;  // LessEqual
    fail.front.fail = 1;     // Zero
    EXPECT_TRUE(invalidates(fail, true, false, false));
    raw::DepthStencilDesc pass = depth(kLess, true);
    pass.front.compare = 2;  // Equal
    pass.front.pass = 2;     // Replace: written only where the depth test passes
    EXPECT_FALSE(invalidates(pass, true, false, false));
    EXPECT_FALSE(invalidates(zfail, false, false, false));  // no stencil aspect: never tested
}

TEST(Zcull, NothingWithoutARegion)
{
    raw::ZcullPass z;
    EXPECT_FALSE(raw::zcullInvalidates(z, depth(kAlways, true), true, false, true, true));
}

}  // namespace
