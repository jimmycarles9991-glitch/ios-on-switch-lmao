// SPDX-License-Identifier: MIT
// raw/runtime/raw_multisample.h and raw_sample_positions.h on the host: a stored multisampled texture's header is NIL's (the
// rows of raw_layout_test's NIL multisampled images), its target the sample grid NVK renders, and a pass's sample layout the
// methods NVK writes, at the standard positions or at the pass's own, rounded where Metal samples them.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "../raw/runtime/raw_multisample.h"
#include "../raw/runtime/raw_target.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"

namespace {

struct Method {
    uint32_t method, value;

    bool operator==(const Method &o) const
    {
        return method == o.method && value == o.value;
    }
};

std::vector<Method> methods(const raw::SampleLayout &l, uint64_t root)
{
    std::vector<uint32_t> words(64);
    raw::Push p{words.data(), words.data() + words.size()};
    raw::sampleLayoutMethods(p, l, 0x800, root);
    switch_gm20b_method decoded[64];
    const long n = switch_gm20b_push_decode(words.data(), size_t(p.at - words.data()), decoded, 64);
    std::vector<Method> out;
    for (long i = 0; i < n; i++) {
        EXPECT_EQ(decoded[i].subchannel, 0);
        out.push_back({decoded[i].method, decoded[i].value});
    }
    return out;
}

raw::SamplePositions positions(std::vector<float> xy, uint32_t samples)
{
    raw::SamplePositions p;
    EXPECT_TRUE(raw::takeSamplePositions(xy.data(), uint32_t(xy.size() / 2), samples, p));
    return p;
}

TEST(SamplePositions, RoundedAsMetalSamplesThem)
{
    EXPECT_EQ(raw::positionSixteenths(0.0f), 0);
    EXPECT_EQ(raw::positionSixteenths(0.03125f), 1);  // a tie, up
    EXPECT_EQ(raw::positionSixteenths(0.53f), 8);
    EXPECT_EQ(raw::positionSixteenths(0.53125f), 9);
    EXPECT_EQ(raw::positionSixteenths(0.55f), 9);
    EXPECT_EQ(raw::positionSixteenths(0.5624f), 9);
    EXPECT_EQ(raw::positionSixteenths(0.9375f), 15);
    EXPECT_EQ(raw::positionSixteenths(0.97f), 15);
    EXPECT_EQ(raw::positionSixteenths(0.99f), 15);
}

TEST(SamplePositions, TakenAtTheCountsMetalTakes)
{
    raw::SamplePositions p;
    const float four[] = {0.0625f, 0.0625f, 0.9375f, 0.1875f, 0.25f, 0.8125f, 0.6875f, 0.5f};
    ASSERT_TRUE(raw::takeSamplePositions(four, 4, 4, p));
    EXPECT_EQ(p.count, 4);
    EXPECT_EQ(p.xy[0], 0x11);
    EXPECT_EQ(p.xy[1], 0x3f);
    EXPECT_EQ(p.xy[2], 0xd4);
    EXPECT_EQ(p.xy[3], 0x8b);
    ASSERT_TRUE(raw::takeSamplePositions(four, 2, 4, p));  // fewer than the samples: the rest at the corner
    EXPECT_EQ(p.count, 2);
    EXPECT_EQ(p.xy[2], 0);
    EXPECT_EQ(p.xy[3], 0);
    EXPECT_FALSE(raw::takeSamplePositions(four, 4, 2, p));  // more than the samples
    EXPECT_FALSE(raw::takeSamplePositions(four, 1, 4, p));
    EXPECT_FALSE(raw::takeSamplePositions(four, 3, 4, p));
    EXPECT_FALSE(raw::takeSamplePositions(four, 2, 1, p));
    for (float bad : {1.0f, -0.0001f, NAN}) {
        const float xy[] = {0.5f, 0.5f, bad, 0.25f};
        EXPECT_FALSE(raw::takeSamplePositions(xy, 2, 2, p)) << bad;
        EXPECT_EQ(p.count, 0);
    }
}

// The pass's positions are read at exactly the count it holds (Metal's getSamplePositions:count: asserts on any other
// non-zero count: the stored_msaa scene's 2x pass of its own positions aborted HW Test when 8 were asked of 2)
TEST(SamplePositions, ReadAtTheCountThePassHolds)
{
    const float own[] = {0.125f, 0.875f, 0.8125f, 0.3125f, 0.0625f, 0.0625f, 0.9375f, 0.1875f};
    for (uint32_t held : {2u, 4u}) {
        std::vector<uint32_t> asked;
        const auto read = [&](float *xy, uint32_t n) {
            asked.push_back(n);
            for (uint32_t i = 0; i < 2 * n; i++)
                xy[i] = own[i];
        };
        raw::SamplePositions p;
        EXPECT_TRUE(raw::readSamplePositions(held, 4, read, p));
        EXPECT_EQ(asked, std::vector<uint32_t>{held});
        EXPECT_EQ(p.count, held);
    }
    for (uint32_t held : {0u, 9u, 16u}) {
        bool called = false;
        raw::SamplePositions p;
        EXPECT_FALSE(raw::readSamplePositions(
            held, 4,
            [&](float *, uint32_t) {
                called = true;
            },
            p));
        EXPECT_FALSE(called) << held;
    }
}

TEST(SampleLayout, StandardPositionsUnchanged)
{
    for (uint32_t samples : {1u, 2u, 4u, 8u}) {
        raw::SampleLayout standard, at;
        ASSERT_TRUE(raw::sampleLayout(samples, standard));
        ASSERT_TRUE(raw::sampleLayoutAt(samples, raw::SamplePositions(), at));
        EXPECT_EQ(at.antiAlias, standard.antiAlias);
        for (int w = 0; w < 4; w++)
            EXPECT_EQ(at.positions[w], standard.positions[w]);
        for (uint32_t w = 0; w < raw::kRootSampleInfoWords; w++)
            EXPECT_EQ(at.root[w], standard.root[w]);
    }
}

// NVK's sixteen locations of a 2x2 quad of pixels each at the pass's positions, and the root table's eight locations; the
// sample masks after them stay the layout's
TEST(SampleLayout, OwnPositionsAsNvkWritesThem)
{
    raw::SampleLayout four, two;
    ASSERT_TRUE(raw::sampleLayoutAt(
        4, positions({0.0625f, 0.0625f, 0.9375f, 0.1875f, 0.25f, 0.8125f, 0.6875f, 0.5f}, 4), four));
    EXPECT_EQ(four.antiAlias, 2u);
    for (int w = 0; w < 4; w++)
        EXPECT_EQ(four.positions[w], 0x8bd43f11u);
    EXPECT_EQ(four.root[0], 0x8bd43f11u);
    EXPECT_EQ(four.root[1], 0u);
    for (uint32_t w = 2; w < raw::kRootSampleInfoWords; w++)
        EXPECT_EQ(four.root[w], 0xffffffffu);
    ASSERT_TRUE(raw::sampleLayoutAt(2, positions({0.125f, 0.875f, 0.8125f, 0.3125f}, 2), two));
    EXPECT_EQ(two.antiAlias, 5u);
    for (int w = 0; w < 4; w++)
        EXPECT_EQ(two.positions[w], 0x5de25de2u);
    EXPECT_EQ(two.root[0], 0x5de2u);
}

TEST(SampleLayout, MethodStream)
{
    raw::SampleLayout l;
    ASSERT_TRUE(
        raw::sampleLayoutAt(4, positions({0.0625f, 0.0625f, 0.9375f, 0.1875f, 0.25f, 0.8125f, 0.6875f, 0.5f}, 4), l));
    const std::vector<Method> want = {
        {NVB197_SET_ANTI_ALIAS, 2},
        {NVB197_SET_ANTI_ALIAS_SAMPLE_POSITIONS(0), 0x8bd43f11},
        {NVB197_SET_ANTI_ALIAS_SAMPLE_POSITIONS(1), 0x8bd43f11},
        {NVB197_SET_ANTI_ALIAS_SAMPLE_POSITIONS(2), 0x8bd43f11},
        {NVB197_SET_ANTI_ALIAS_SAMPLE_POSITIONS(3), 0x8bd43f11},
        {NVB197_SET_CONSTANT_BUFFER_SELECTOR_A, 0x800},
        {NVB197_SET_CONSTANT_BUFFER_SELECTOR_B, 0x12},
        {NVB197_SET_CONSTANT_BUFFER_SELECTOR_C, 0x34567800},
        {NVB197_LOAD_CONSTANT_BUFFER_OFFSET, 0x1c},
        {NVB197_LOAD_CONSTANT_BUFFER(0), 0x8bd43f11},
        {NVB197_LOAD_CONSTANT_BUFFER(0), 0},
        {NVB197_LOAD_CONSTANT_BUFFER(0), 0xffffffff},
        {NVB197_LOAD_CONSTANT_BUFFER(0), 0xffffffff},
        {NVB197_LOAD_CONSTANT_BUFFER(0), 0xffffffff},
        {NVB197_LOAD_CONSTANT_BUFFER(0), 0xffffffff},
    };
    EXPECT_EQ(methods(l, 0x1234567800ull), want);
}

// raw_layout_test's NIL rows: a 720x1280 RGBA8 image of 4 samples and a 1280x720 one of 2, viewed at 0x100000
TEST(MultisampleHeader, NilsHeader)
{
    uint32_t w[8];
    ASSERT_TRUE(raw::multisampleHeader(720, 1280, 4, 4, 0x100000, 0x58d24908, false, w));
    const uint32_t four[8] = {0x58d24908, 0x00100000, 0x00600000, 0x00070020,
                              0xe08002cf, 0x800004ff, 0x03000000, 0x00000200};
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(w[i], four[i]) << "word " << i;
    ASSERT_TRUE(raw::multisampleHeader(1280, 720, 2, 4, 0x100000, 0x58d24908, false, w));
    const uint32_t two[8] = {0x58d24908, 0x00100000, 0x00600000, 0x00070020,
                             0xe08004ff, 0x800002cf, 0x03000000, 0x00000500};
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(w[i], two[i]) << "word " << i;
    EXPECT_FALSE(raw::multisampleHeader(64, 64, 1, 4, 0x100000, 0x58d24908, false, w));
    EXPECT_FALSE(raw::multisampleHeader(64, 64, 3, 4, 0x100000, 0x58d24908, false, w));
}

// A Depth32Float texture of 2 samples sampled (depth2d_ms): Depth32Float's word 0 (raw_zeta.h), its size in pixels,
// MULTI_SAMPLE_COUNT 2X1_D3D
TEST(MultisampleHeader, DepthOfTwoSamples)
{
    uint32_t w[8];
    ASSERT_TRUE(raw::multisampleHeader(64, 48, 2, 4, 0x200000, 0x7017ffaf, false, w));
    EXPECT_EQ(w[0], 0x7017ffafu);
    EXPECT_EQ(w[4] & 0xffff, 63u);
    EXPECT_EQ(w[5] & 0xffff, 47u);
    EXPECT_EQ(w[7] >> 8 & 0xf, 5u);
}

// The target a pass renders a stored 32x32 RGBA8 texture of 4 samples through: its sample grid, 64 samples a row, 64 rows
// (NVK's row_stride_el and level_extent_sa.height)
TEST(MultisampleTarget, TheSampleGrid)
{
    raw::fx::MipChain c;
    ASSERT_TRUE(raw::fx::msLayout(32, 32, 4, 4, c));

    struct {
        uint32_t height;
        uint8_t samples;
    } image = {32, 4};

    raw::ColorTargetDesc d;
    d.va = 0x400000;
    d.pitch = c.level[0].rowBytes;
    d.height = raw::sampleRows(image);
    d.bpp = 4;
    d.rtFormat = 0xd5;
    d.blockLinear = true;
    d.blockHeight = c.level[0].yLog2;
    d.layerBytes = uint64_t(c.level[0].rowBytes) * c.level[0].rows;
    const switch_gm20b_target k = raw::kitTarget(d);
    EXPECT_EQ(k.width, 64u);
    EXPECT_EQ(k.height, 64u);
    EXPECT_EQ(k.block_height_log2, 2u);
    EXPECT_EQ(k.array_pitch, 256u * 64 / 4);
}

}  // namespace
