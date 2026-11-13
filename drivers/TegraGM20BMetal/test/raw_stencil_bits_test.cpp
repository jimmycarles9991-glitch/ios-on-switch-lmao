// SPDX-License-Identifier: MIT
// raw/runtime/raw_stencil_bits.h on the host. Each bit's draw state as the GPU's depth/stencil registers, then the draws run on a
// model of the ROP (Replace through the write mask, nothing for a discarded fragment) and of the programs (pack/stencil_bits.metal:
// discard where the draw's bit of the source texel at its map is clear): over a cleared target they rebuild every stencil value,
// and at the turn copies' maps they put each texel where Metal stores it and back.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../raw/runtime/raw_depth_stencil_state.h"
#include "../raw/runtime/raw_stencil_bits.h"

namespace {

using raw::Rotation;
using raw::StencilSample;
using raw::TurnMap;

struct Image {
    uint32_t width, height;
    std::vector<uint8_t> texels;

    uint8_t &at(int32_t x, int32_t y)
    {
        return texels[size_t(y) * width + size_t(x)];
    }
};

// The draws into `target`, cleared to 0 first, from `source` at `map`, as the GPU runs them
void drawBits(Image &target, Image &source, const TurnMap &map)
{
    for (uint8_t &t : target.texels)
        t = 0;
    const bool drawn = raw::forEachStencilBit(
        raw::stencilBitsArgs(map, 1, StencilSample::Zero), [&](uint32_t k, const raw::StencilBitsArgs &a) {
            const raw::DepthStencilRegs r = raw::depthStencilRegs(raw::stencilBitState(k), raw::kStencilBitReference,
                                                                  raw::kStencilBitReference, true, true);
            const uint32_t ref = r.v[raw::kFrontStencilReference], writeMask = r.v[raw::kFrontStencilReference + 2];
            for (int32_t y = 0; y < int32_t(target.height); y++)
                for (int32_t x = 0; x < int32_t(target.width); x++) {
                    const uint8_t s =
                        source.at(a.map.ax * x + a.map.ay * y + a.map.cx, a.map.bx * x + a.map.by * y + a.map.cy);
                    if (s & a.bit)
                        target.at(x, y) = uint8_t((target.at(x, y) & ~writeMask) | (ref & writeMask));
                }
            return true;
        });
    EXPECT_TRUE(drawn);
}

Rotation turned(uint8_t turn, uint32_t w, uint32_t h)
{
    Rotation r;
    r.turn = turn;
    r.width = w;
    r.height = h;
    return r;
}

// SET_STENCIL_* of bit k's draw: the test on, compare ALWAYS, Replace on pass and Keep otherwise, reference 0xff, write mask
// the bit alone, both faces; depth neither tested nor written
TEST(RawStencilBits, EachBitsStateWritesThatBitAlone)
{
    for (const bool depth : {false, true})
        for (uint32_t k = 0; k < raw::kStencilBits; k++) {
            const raw::DepthStencilRegs r = raw::depthStencilRegs(raw::stencilBitState(k), raw::kStencilBitReference,
                                                                  raw::kStencilBitReference, depth, true);
            EXPECT_EQ(r.v[0], 0u);  // SET_DEPTH_TEST
            EXPECT_EQ(r.v[1], 0u);  // SET_DEPTH_WRITE
            EXPECT_EQ(r.v[3], 1u);  // SET_STENCIL_TEST
            for (const uint32_t ops : {4u, 11u}) {
                EXPECT_EQ(r.v[ops], 1u);          // FAIL: KEEP
                EXPECT_EQ(r.v[ops + 1], 1u);      // ZFAIL: KEEP
                EXPECT_EQ(r.v[ops + 2], 3u);      // ZPASS: REPLACE
                EXPECT_EQ(r.v[ops + 3], 0x207u);  // FUNC: ALWAYS
            }
            EXPECT_EQ(r.v[raw::kFrontStencilReference], 0xffu);
            EXPECT_EQ(r.v[raw::kFrontStencilReference + 2], 1u << k);  // SET_STENCIL_MASK
            EXPECT_EQ(r.v[raw::kBackStencilReference], 0xffu);
            EXPECT_EQ(r.v[raw::kBackStencilReference + 1], 1u << k);  // SET_BACK_STENCIL_MASK
        }
}

// Eight draws, one a bit, each with its bit, the map, the sample count and the fixed sample; a refused draw ends the loop
TEST(RawStencilBits, TheLoopDrawsEachBitOnce)
{
    const TurnMap map = raw::planeToFrame(turned(3, 96, 64));
    std::vector<uint32_t> bits;
    EXPECT_TRUE(raw::forEachStencilBit(raw::stencilBitsArgs(map, 4, StencilSample::Last),
                                       [&](uint32_t k, const raw::StencilBitsArgs &a) {
                                           EXPECT_EQ(a.bit, 1u << k);
                                           EXPECT_EQ(a.samples, 4u);
                                           EXPECT_EQ(a.sample, 3u);
                                           EXPECT_EQ(a.map.ax, map.ax);
                                           EXPECT_EQ(a.map.cy, map.cy);
                                           bits.push_back(a.bit);
                                           return true;
                                       }));
    EXPECT_EQ(bits, (std::vector<uint32_t>{1, 2, 4, 8, 16, 32, 64, 128}));
    EXPECT_EQ(raw::stencilBitsArgs(map, 4, StencilSample::Zero).sample, 0u);
    EXPECT_EQ(raw::stencilBitsArgs(map, 2, StencilSample::LeastDepth).sample, 0u);
    uint32_t draws = 0;
    EXPECT_FALSE(raw::forEachStencilBit(raw::StencilBitsArgs{}, [&](uint32_t k, const raw::StencilBitsArgs &) {
        draws++;
        return k < 2;
    }));
    EXPECT_EQ(draws, 3u);
}

// Over a cleared target the draws rebuild every stencil value
TEST(RawStencilBits, TheDrawsCopyEveryValue)
{
    Image source{256, 1, {}}, target{256, 1, std::vector<uint8_t>(256, 0x96)};
    for (uint32_t v = 0; v < 256; v++)
        source.texels.push_back(uint8_t(v));
    drawBits(target, source, TurnMap{});
    EXPECT_EQ(target.texels, source.texels);
}

// A frame's stencil into the plane at frameToPlane lands where Metal stores each texel; back at planeToFrame it is the frame's
TEST(RawStencilBits, TheTurnMapsMoveTheStencilAndBack)
{
    for (uint8_t turn = 1; turn <= 4; turn++) {
        const Rotation r = turned(turn, 96, 64);
        Image frame{r.width, r.height, {}}, plane{r.planeWidth(), r.planeHeight(), {}}, back{r.width, r.height, {}};
        for (uint32_t i = 0; i < r.width * r.height; i++)
            frame.texels.push_back(uint8_t(i * 37 + i / 96));
        plane.texels.resize(plane.width * plane.height);
        back.texels.resize(frame.texels.size());
        drawBits(plane, frame, raw::frameToPlane(r));
        for (uint32_t y = 0; y < r.height; y += 3)
            for (uint32_t x = 0; x < r.width; x += 5) {
                const raw::Rect p = r.rect({x, y, x + 1, y + 1});
                EXPECT_EQ(plane.at(int32_t(p.x0), int32_t(p.y0)), frame.at(int32_t(x), int32_t(y))) << int(turn);
            }
        drawBits(back, plane, raw::planeToFrame(r));
        EXPECT_EQ(back.texels, frame.texels) << int(turn);
    }
}

// The sample a resolve takes, as an M-series Mac's GPU resolves (hw_test_app gpu.raw.stored_msaa): Sample0 sample 0;
// DepthResolvedSample the depth resolve's filter's beside one, the last sample beside a depth that does not resolve, sample
// 0 without depth
TEST(RawStencilBits, ResolveSampleFollowsTheDepthResolve)
{
    EXPECT_EQ(raw::stencilResolveSample(0, 1, true, true), StencilSample::Zero);
    EXPECT_EQ(raw::stencilResolveSample(1, 0, true, true), StencilSample::Zero);
    EXPECT_EQ(raw::stencilResolveSample(1, 1, true, true), StencilSample::LeastDepth);
    EXPECT_EQ(raw::stencilResolveSample(1, 2, true, true), StencilSample::GreatestDepth);
    EXPECT_EQ(raw::stencilResolveSample(1, 1, true, false), StencilSample::Last);
    EXPECT_EQ(raw::stencilResolveSample(0, 1, true, false), StencilSample::Zero);
    EXPECT_EQ(raw::stencilResolveSample(1, 2, false, false), StencilSample::Zero);
}

// A state for each stencil format and sample, none for a depth-only format or a depth pick of a single sample
TEST(RawStencilBits, StatesOfEachFormatAndSample)
{
    EXPECT_STREQ(raw::stencilBitsState(253, 1, StencilSample::Zero), "raw.stencil_bits.stencil_bits_s8_frag");
    EXPECT_STREQ(raw::stencilBitsState(260, 1, StencilSample::Zero), "raw.stencil_bits.stencil_bits_d32fs8_frag");
    EXPECT_STREQ(raw::stencilBitsState(253, 4, StencilSample::Zero), "raw.stencil_bits.stencil_bits_ms_s8_frag");
    EXPECT_STREQ(raw::stencilBitsState(260, 2, StencilSample::Zero), "raw.stencil_bits.stencil_bits_ms_d32fs8_frag");
    EXPECT_STREQ(raw::stencilBitsState(260, 4, StencilSample::Last), "raw.stencil_bits.stencil_bits_ms_d32fs8_frag");
    EXPECT_STREQ(raw::stencilBitsState(260, 4, StencilSample::LeastDepth),
                 "raw.stencil_bits.stencil_bits_ms_d32fs8_min_frag");
    EXPECT_STREQ(raw::stencilBitsState(260, 4, StencilSample::GreatestDepth),
                 "raw.stencil_bits.stencil_bits_ms_d32fs8_max_frag");
    EXPECT_EQ(raw::stencilBitsState(253, 4, StencilSample::LeastDepth), nullptr);
    EXPECT_EQ(raw::stencilBitsState(260, 1, StencilSample::GreatestDepth), nullptr);
    EXPECT_EQ(raw::stencilBitsState(252, 1, StencilSample::Zero), nullptr);
    EXPECT_EQ(raw::stencilViewWord0(252), 0u);
    EXPECT_EQ(raw::stencilViewWord0(253), raw::kZetaFormatInfo[raw::zetaFormatIndex(253)].tic0);
}

// Every zeta format's aspects move through turn copies; no other format is a zeta's
TEST(RawStencilBits, TurnCopiesMoveEveryZetaFormat)
{
    for (const uint32_t format : {250u, 252u, 253u, 260u})
        EXPECT_TRUE(raw::turnMovesZeta(format)) << format;
    EXPECT_FALSE(raw::turnMovesZeta(80));
}

}  // namespace
