// SPDX-License-Identifier: MIT
// Copyright © 2025 Valve Corporation (the reference regions: Mesa src/nouveau/nil/zcull.rs tests, dumped from NVIDIA's
// driver)
// raw/runtime/raw_zcull.h on the host: a region's subregions are nil_zcull_new's (its reference table, for a desktop GPU's
// layout), its words fit kZcullRegionWords, and a pass's draws invalidate it exactly where zcull cannot follow them.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../raw/runtime/raw_zcull.h"
#include "../raw/runtime/raw_zcull_planes.h"

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

raw::ZcullAsk clearedDepth()
{
    raw::ZcullAsk a;
    a.on = a.depth = a.cleared = true;
    return a;
}

// gpu.raw.zcull msaa4x_passes: the rule reads no sample count
TEST(Zcull, AClearedDepthTakesARegion)
{
    EXPECT_TRUE(raw::zcullCandidate(clearedDepth()));
    raw::ZcullAsk a = clearedDepth();
    a.on = false;
    EXPECT_FALSE(raw::zcullCandidate(a));
    a = clearedDepth();
    a.depth = false;
    EXPECT_FALSE(raw::zcullCandidate(a));
    a = clearedDepth();
    a.atStorageBase = false;
    EXPECT_FALSE(raw::zcullCandidate(a));
}

// gpu.raw.zcull layered_passes: the region as deep as the pass's layers, each layer its share of the zcull RAM
TEST(Zcull, ALayeredPassTakesARegionAsDeepAsItsLayers)
{
    raw::ZcullAsk a = clearedDepth();
    a.layers = a.zetaLayers = 2;
    EXPECT_TRUE(raw::zcullCandidate(a));
    a.zetaLayers = 6;
    EXPECT_TRUE(raw::zcullCandidate(a));
    a.zetaLayers = 1;
    EXPECT_FALSE(raw::zcullCandidate(a));
    const TegraGM20BAbiZcullInfo x1 = {32, 32, 1024, 2048, 32, 32, 192, 16, 32, 64, 0x1500, 0x1000};
    raw::ZcullRegion two;
    ASSERT_TRUE(raw::zcullRegion(x1, 0, 0, 1280, 720, two, 2));
    EXPECT_EQ(two.layers, 2u);
    EXPECT_LE(two.aliquotCount * 2, x1.aliquot_total);
    std::vector<uint32_t> words(raw::kZcullRegionWords + 8);
    raw::Push p{words.data(), words.data() + words.size()};
    raw::zcullRegionWords(p, two, 252, raw::kZcullLess);
    bool location = false, size = false;
    for (uint32_t *w = words.data(); w + 2 < p.at; w++) {
        location |= w[1] == two.aliquotCount * 2 << 16 && w[2] == two.aliquotCount;
        size |= w[1] == two.width && w[2] == two.height && w[3] == 2;
    }
    EXPECT_TRUE(location) << "SET_ZCULL_REGION_LOCATION's count is every layer's, _ALIQUOTS one layer's";
    EXPECT_TRUE(size) << "SET_ZCULL_REGION_SIZE_C: the layers";
    raw::ZcullRegion many;
    EXPECT_FALSE(raw::zcullRegion(x1, 0, 0, 1280, 720, many, 4096)) << "no layer's share of the RAM holds a subregion";
}

// gpu.raw.zcull loaded_passes=2: a pass that clears and stores a depth a recorded pass loads stores its region; the
// next pass that loads the depth over the same region loads it, unless something wrote the depth in between
TEST(Zcull, ALoadedDepthLoadsTheRegionAnEarlierPassStored)
{
    const TegraGM20BAbiZcullInfo x1 = {32, 32, 1024, 2048, 32, 32, 192, 16, 32, 64, 0x1500, 0x1000};
    constexpr uint64_t kZeta = 0x100000, kZetaBytes = 0x400000, kStorage = 0x2000000;
    uint64_t asked = 0;
    const auto storage = [&](uint64_t bytes) {
        asked = bytes;
        return kStorage;
    };
    raw::ZcullPlanes planes;
    raw::ZcullPass first;
    raw::zcullBegin(first, clearedDepth(), x1, 0, 0, 1280, 720, 1, planes.held(kZeta), true, storage);
    ASSERT_TRUE(first.candidate);
    EXPECT_FALSE(first.load);
    ASSERT_TRUE(first.store);
    EXPECT_EQ(first.storage, kStorage);
    EXPECT_EQ(asked, uint64_t(first.region.aliquotCount) * 32 + 32 + 192);
    planes.wrote(kZeta, kZetaBytes);  // the first pass's own write, at its begin
    planes.stored(raw::zcullStoredBy(first, kZeta, kZeta, kZetaBytes));

    raw::ZcullAsk loaded = clearedDepth();
    loaded.cleared = false;
    raw::ZcullPass second;
    const raw::ZcullStored held = planes.held(kZeta);
    raw::zcullBegin(second, loaded, x1, 0, 0, 1280, 720, 1, held, true, storage);
    ASSERT_TRUE(second.candidate);
    EXPECT_TRUE(second.load);
    EXPECT_TRUE(second.store) << "into the storage it loaded";
    EXPECT_EQ(second.storage, kStorage);
    EXPECT_EQ(second.dir, raw::kZcullLess);

    raw::ZcullPass smaller;
    raw::zcullBegin(smaller, loaded, x1, 0, 0, 640, 720, 1, held, false, storage);
    EXPECT_FALSE(smaller.candidate) << "another region";
    raw::ZcullPass none;
    raw::zcullBegin(none, loaded, x1, 0, 0, 1280, 720, 1, raw::ZcullStored(), false, storage);
    EXPECT_FALSE(none.candidate) << "nothing stored";
    planes.wrote(kZeta + kZetaBytes - 4, 4);
    EXPECT_EQ(planes.held(kZeta).zetaVa, 0u) << "a write to the depth's memory";
    planes.stored(raw::zcullStoredBy(first, kZeta, kZeta, kZetaBytes));
    planes.wrote(kZeta + kZetaBytes, 64);
    EXPECT_EQ(planes.held(kZeta).zetaVa, kZeta) << "a write past it";
    planes.clear();
    EXPECT_EQ(planes.held(kZeta).zetaVa, 0u) << "the next command buffer";

    raw::ZcullPass unkept;
    raw::zcullBegin(unkept, clearedDepth(), x1, 0, 0, 1280, 720, 1, raw::ZcullStored(), false, storage);
    EXPECT_TRUE(unkept.candidate);
    EXPECT_FALSE(unkept.store) << "no recorded pass loads the depth";
    EXPECT_EQ(unkept.storage, 0u);
    raw::ZcullPass full;
    raw::zcullBegin(full, clearedDepth(), x1, 0, 0, 1280, 720, 1, raw::ZcullStored(), true, [](uint64_t) {
        return uint64_t(0);
    });
    EXPECT_TRUE(full.candidate);
    EXPECT_FALSE(full.store) << "no storage to be had";

    raw::ZcullPass reversed = first;
    reversed.dir = raw::kZcullGreater;
    EXPECT_EQ(raw::zcullStoredBy(reversed, kZeta, kZeta, kZetaBytes).dir, raw::kZcullGreater);
    planes.stored(raw::zcullStoredBy(first, kZeta, 0, 0));
    EXPECT_EQ(planes.held(kZeta).zetaVa, 0u) << "a depth whose memory no write could be matched against";
    std::vector<uint32_t> words(raw::kZcullRegionWords + 8);
    raw::Push p{words.data(), words.data() + words.size()};
    raw::zcullRegionWords(p, second.region, 252, second.dir, second.storage, second.storageBytes);
    bool storageWords = false;
    for (uint32_t *w = words.data(); w + 4 < p.at; w++)
        storageWords |=
            w[1] == 0 && w[2] == uint32_t(kStorage) && w[3] == 0 && w[4] == uint32_t(kStorage + second.storageBytes);
    EXPECT_TRUE(storageWords) << "SET_ZCULL_STORAGE_A..D: the storage and its end";
}

TEST(Zcull, NothingWithoutARegion)
{
    raw::ZcullPass z;
    EXPECT_FALSE(raw::zcullInvalidates(z, depth(kAlways, true), true, false, true, true));
}

}  // namespace
