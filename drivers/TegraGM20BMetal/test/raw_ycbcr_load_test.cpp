// SPDX-License-Identifier: MIT
// raw/runtime/raw_ycbcr_load.h: a pass that loads a two-plane YCbCr target. The tile value of each read (the nearest half, a
// tie up), and the planes after a loaded pass's draw by the raw path's route (load state, draws, tile-masked store) against
// an M4 Max's own passes: the hashes raw/test/ycbcrloadoracle.mm golden printed of Metal's planes (that oracle holds the
// route's states, run on the Mac's GPU, to every byte of 1456 passes over every format and surface).
#include "raw_ycbcr_load_cases.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

const uint16_t kKnots[513] = {
#include "../raw/pack/srgb_knots.h"
};

// The half nearest num / den by exhaustive search over the halves in [0, 1], a tie the larger
double nearestHalf(uint32_t num, uint32_t den)
{
    double best = 0, bestOff = 2;
    for (uint32_t bits = 0; bits <= 0x3c00; bits++) {
        _Float16 h;
        const uint16_t b = uint16_t(bits);
        memcpy(&h, &b, 2);
        const double off = std::fabs(double(h) * den - double(num));
        if (off < bestOff || (off == bestOff && double(h) > best)) {
            best = double(h);
            bestOff = off;
        }
    }
    return best;
}

TEST(RawYcbcrLoad, TheTileHoldsEachReadAsTheNearestHalfATieUp)
{
    for (const uint32_t den : {255u, 1023u, 2048u, 8192u})
        for (uint32_t num = 0; num <= den; num++)
            ASSERT_EQ(double(raw::ycbcrLoadHalf(num, den)), nearestHalf(num, den)) << num << " / " << den;
    // an M4 Max's fetches of YCBCR8_420_2P_sRGB's steps (ties) beside their reads
    EXPECT_EQ(raw::ycbcrLoadHalf(7554, 8192), 7556.0f / 8192);
    EXPECT_EQ(raw::ycbcrLoadHalf(4106, 8192), 4108.0f / 8192);
    EXPECT_EQ(raw::ycbcrLoadHalf(2441, 8192), 2442.0f / 8192);
    EXPECT_EQ(raw::ycbcrLoadHalf(3549, 8192), 0.433349609f);
}

struct Golden {
    ycbcrload::Case c;
    uint64_t metal;
};

// raw/test/ycbcrloadoracle.mm golden on an M4 Max under macOS 27.2: Metal's planes after kGoldenArm
const Golden kGolden[] = {
    {{540, ycbcrload::fourcc("420v"), nullptr}, 0x11baa8cf6c3e05b9ull},
    {{500, ycbcrload::fourcc("420f"), "ITU_R_709_2"}, 0x93101cdce779635dull},
    {{520, ycbcrload::fourcc("420v"), nullptr}, 0x1b55873bc381acdaull},
    {{545, ycbcrload::fourcc("x444"), nullptr}, 0x763b349863a7149full},
    {{503, ycbcrload::fourcc("444f"), nullptr}, 0xa37b52d89dc171b7ull},
    {{541, ycbcrload::fourcc("422v"), nullptr}, 0x96cf2d54459e13a9ull},
    {{543, ycbcrload::fourcc("xf20"), nullptr}, 0xf9c567a96d4fd656ull},
    {{525, ycbcrload::fourcc("x420"), "ITU_R_2020"}, 0x329f4c9f31b04da9ull},
    {{527, ycbcrload::fourcc("xf44"), nullptr}, 0x8d5804ad7f5b74cdull},
    {{506, ycbcrload::fourcc("x422"), "ITU_R_709_2"}, 0x31e83eb45b2f622bull},
};

// A surface's facts of `c` at w x h: rows 64-byte aligned, plane 1 after plane 0
raw::YcbcrFacts factsOf(const ycbcrload::Case &c, uint32_t w, uint32_t h)
{
    const bool tenBit = (c.fourcc >> 24) == 'x' || (c.fourcc >> 24) == 'p';
    const uint32_t pitch = (w * (tenBit ? 2 : 1) * 2 + 63) / 64 * 64;
    raw::YcbcrFacts f;
    f.pixelFormat = c.format;
    f.fourcc = c.fourcc;
    f.planes = 2;
    f.width = f.surfaceWidth = w;
    f.height = f.surfaceHeight = h;
    f.offset[1] = uint64_t(pitch) * h;
    f.pitch[0] = f.pitch[1] = pitch;
    f.allocBytes = 2 * f.offset[1];
    f.simple = true;
    f.target = true;
    f.matrix = c.matrix;
    return f;
}

struct Pass {
    raw::YcbcrPlan plan;
    ycbcrload::Layout layout;
    ycbcrload::Planes before;
    std::vector<float> values;
};

Pass goldenPass(const ycbcrload::Case &c)
{
    const ycbcrload::Size size = ycbcrload::sizes(true)[0];
    Pass p;
    EXPECT_EQ(raw::planYcbcr(factsOf(c, size[0], size[1]), p.plan), nullptr);
    p.layout = ycbcrload::layoutOf(p.plan, size[0], size[1]);
    p.before = ycbcrload::randomPlanes(p.layout, c.format * 131 + size[0]);
    p.values = ycbcrload::drawnValues(size[0] * size[1], c.format * 7 + size[1]);
    return p;
}

TEST(RawYcbcrLoad, ALoadedPassStoresTheTilesItsDrawReachesAsMetalDoes)
{
    for (const Golden &g : kGolden) {
        const Pass p = goldenPass(g.c);
        const ycbcrload::Planes after =
            ycbcrload::modelPass(g.c, p.plan, p.layout, p.before, p.values, ycbcrload::kGoldenArm, kKnots);
        EXPECT_EQ(ycbcrload::hash(after), g.metal) << g.c.format;
    }
}

TEST(RawYcbcrLoad, StoringEveryTileOfALoadedPassIsNotMetals)
{
    for (const Golden &g : kGolden) {
        const Pass p = goldenPass(g.c);
        ycbcrload::Arm every = ycbcrload::kGoldenArm;
        every.second = true;
        const ycbcrload::Planes after = ycbcrload::modelPass(g.c, p.plan, p.layout, p.before, p.values, every, kKnots);
        if (g.c.format != 545)  // 4:4:4 codes come back as they were loaded
            EXPECT_NE(ycbcrload::hash(after), g.metal) << g.c.format;
    }
}

TEST(RawYcbcrLoad, ALoadedPassWithoutADrawStoresNothing)
{
    for (const Golden &g : kGolden) {
        const Pass p = goldenPass(g.c);
        ycbcrload::Arm none;
        none.draws = false;
        EXPECT_EQ(ycbcrload::modelPass(g.c, p.plan, p.layout, p.before, p.values, none, kKnots), p.before);
    }
}

TEST(RawYcbcrLoad, EveryLoadedFormatHasALoadStateAndAMaskedStore)
{
    for (const ycbcrload::Case &c : ycbcrload::cases()) {
        raw::YcbcrPlan plan;
        const raw::YcbcrFacts f = factsOf(c, 64, 32);
        if (raw::planYcbcr(f, plan))
            continue;
        EXPECT_NE(raw::ycbcrLoadState(plan.output), nullptr) << c.format;
        EXPECT_NE(raw::ycbcrMaskedStoreState(false, plan.store), nullptr) << c.format;
        EXPECT_NE(raw::ycbcrMaskedStoreState(true, plan.store), nullptr) << c.format;
    }
}

// A Load of a 420v target's planes as the M4 Max's tile held them (raw/test/ycbcrloadoracle.mm's probe of 16 x 8 texels):
// red Cr, green luma, blue Cb, the chroma between blocks rounded once, half up
TEST(RawYcbcrLoad, ALoadHoldsTheChromaBetweenBlocksRoundedOnce)
{
    constexpr uint32_t W = 16, H = 8;
    std::vector<uint16_t> luma(W * H), chroma(W * H / 2);
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++)
            luma[y * W + x] = uint16_t(16 + (x * 13 + y * 29) % 220);
    for (uint32_t y = 0; y < H / 2; y++)
        for (uint32_t x = 0; x < W; x++)
            chroma[y * W + x] = uint16_t(16 + (x * 37 + y * 53) % 224);
    std::vector<float> tile(W * H * 4);
    raw::ycbcrLoaded({255, true}, luma.data(), chroma.data(), W, H, tile.data());
    const auto code = [&](uint32_t x, uint32_t y, int c) {
        return std::lround(tile[4 * (y * W + x) + c] * 255);
    };
    EXPECT_EQ(code(0, 0, 0), 53);  // an edge texel: its block's own Cr
    EXPECT_EQ(code(1, 0, 0), 72);  // 71.5 across, half up
    EXPECT_EQ(code(2, 0, 0), 109);
    EXPECT_EQ(code(5, 0, 0), 164);
    EXPECT_EQ(code(0, 1, 0), 66);
    EXPECT_EQ(code(0, 1, 2), 29);
    EXPECT_EQ(code(1, 1, 0), 85);  // 84.75 over four blocks
    EXPECT_EQ(code(15, 0, 0), 123);
    EXPECT_EQ(code(3, 1, 1), 84);
    EXPECT_EQ(tile[3], 1.0f);
    EXPECT_EQ(tile[4 * 17 + 0], float(_Float16(85.0f / 255)));
}

// The same 420v planes loaded and drawn over through write mask green: the chroma the store averages is the loaded mix, so
// a block reloaded and stored moves (block 0's Cr 53 becomes the mean of 53, 72, 66 and 85 rounded up, 69), and the luma
// is the drawn green's code
TEST(RawYcbcrLoad, ALumaOnlyDrawOverALoadStoresTheLoadedChromasMean)
{
    constexpr uint32_t W = 16, H = 8;
    std::vector<uint16_t> luma(W * H), chroma(W * H / 2), lumaOut(W * H), chromaOut(W * H / 2);
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++)
            luma[y * W + x] = uint16_t(16 + (x * 13 + y * 29) % 220);
    for (uint32_t y = 0; y < H / 2; y++)
        for (uint32_t x = 0; x < W; x++)
            chroma[y * W + x] = uint16_t(16 + (x * 37 + y * 53) % 224);
    std::vector<float> green(W * H);
    for (uint32_t x = 0; x < W; x++)
        for (uint32_t y = 0; y < H; y++)
            green[y * W + x] = (float(x) + 0.5f) * 0.0123f;
    ASSERT_TRUE(raw::ycbcrStoreOverLoad({255, true}, luma.data(), chroma.data(), green.data(), W, H, lumaOut.data(),
                                        chromaOut.data()));
    EXPECT_EQ(chroma[1], 53u);
    EXPECT_EQ(chromaOut[1], 69u);
    EXPECT_EQ(lumaOut[0], uint16_t(std::floor(double(_Float16(0.5f * 0.0123f)) * 255 + 0.5)));
    EXPECT_EQ(lumaOut[W * H - 1], uint16_t(std::floor(double(_Float16(15.5f * 0.0123f)) * 255 + 0.5)));
    EXPECT_FALSE(raw::ycbcrStoreOverLoad({255, true}, luma.data(), chroma.data(), green.data(), W - 1, H,
                                         lumaOut.data(), chromaOut.data()));
}

}  // namespace
