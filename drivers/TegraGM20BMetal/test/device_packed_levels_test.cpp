// SPDX-License-Identifier: MIT
// R10Unorm_PACKED of several levels over a 'p420' plane on gm20b::Device (createYcbcrTexture, level_zero.h): an M4 Max
// reads level 0 at every lod while its shader sees every level's size (m2v-textures11 probes/h4/mips.txt). The image is
// a chain of as many levels under one header, and the copy engine copies level 0's top-left corner into each level after.
#include "device_views_harness.h"
#include "../level_zero.h"
#include "../push.h"
#include "../ycbcr.h"
#include <cstdlib>
#include <unistd.h>

namespace {

using namespace views;

constexpr uint32_t kP420 = 0x70343230;

// What the jobs from `first` on pushed: the copy engine's launches, each one's source, destination and line length, and
// the colour targets 0 the passes bound
struct Launch {
    uint64_t from = 0, to = 0;
    uint32_t length = 0;
};

struct Pushed {
    std::vector<Launch> copies;
    std::vector<uint64_t> targets;
};

Pushed pushed(size_t first)
{
    // NVB197_SET_COLOR_TARGET_A/B/FORMAT(0)
    constexpr uint32_t kTargetA = 0x0800, kTargetB = 0x0804, kTargetFormat = 0x0810;
    Pushed out;
    Launch at;
    uint64_t target = 0;
    const auto method = [&](uint32_t subc, uint32_t m, uint32_t v) {
        if (subc == gm20b::SUBC_3D && m == kTargetA)
            target = (target & 0xffffffffull) | uint64_t(v) << 32;
        else if (subc == gm20b::SUBC_3D && m == kTargetB)
            target = (target & ~0xffffffffull) | v;
        else if (subc == gm20b::SUBC_3D && m == kTargetFormat && v)
            out.targets.push_back(target);
        if (subc != gm20b::SUBC_COPY)
            return;
        if (m == NVB0B5_OFFSET_IN_UPPER)
            at.from = (at.from & 0xffffffffull) | uint64_t(v) << 32;
        else if (m == NVB0B5_OFFSET_IN_LOWER)
            at.from = (at.from & ~0xffffffffull) | v;
        else if (m == NVB0B5_OFFSET_OUT_UPPER)
            at.to = (at.to & 0xffffffffull) | uint64_t(v) << 32;
        else if (m == NVB0B5_OFFSET_OUT_LOWER)
            at.to = (at.to & ~0xffffffffull) | v;
        else if (m == NVB0B5_LINE_LENGTH_IN)
            at.length = v;
        else if (m == NVB0B5_LAUNCH_DMA)
            out.copies.push_back(at);
    };
    for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
        const std::vector<uint32_t> &w = kernel_model::model.jobs[j].words;
        for (size_t i = 0; i < w.size();) {
            const uint32_t h = w[i++], op = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                           m = (h & 0xfff) << 2;
            if (op == 4) {
                method(subc, m, count);
                continue;
            }
            for (uint32_t k = 0; k < count && i < w.size(); ++k, ++i)
                method(subc, op == 1 ? m + 4 * k : op == 5 && k ? m + 4 : m, w[i]);
        }
    }
    return out;
}

class DevicePackedLevels : public DeviceViews {
protected:
    void *base = nullptr;
    size_t page = 0;

    void SetUp() override
    {
        DeviceViews::SetUp();
        addOwnState("raw.ycbcr.ycbcr_packed_plane_frag", 90);
        page = size_t(getpagesize());
        base = aligned_alloc(page, page);
    }

    void TearDown() override
    {
        free(base);
        DeviceViews::TearDown();
    }

    // R10Unorm_PACKED 32x16 of `levels` levels over plane 0 of a 'p420' 32x16 of rows 64
    gm20b::YcbcrPlan plan(uint32_t levels)
    {
        gm20b::YcbcrFacts f;
        f.pixelFormat = 588;
        f.fourcc = kP420;
        f.planes = 2;
        f.width = f.surfaceWidth = 32;
        f.height = f.surfaceHeight = 16;
        f.offset[1] = 64 * 16;
        f.pitch[0] = f.pitch[1] = 64;
        f.allocBytes = f.mappedBytes = page;
        f.simple = true;
        f.levels = levels;
        gm20b::YcbcrPlan p;
        EXPECT_EQ(gm20b::planYcbcr(f, p), nullptr);
        return p;
    }
};

// Its header answers three levels of 32x16, 16x8 and 8x4 (MAX_MIP_LEVEL and the view's last level 2), its codes read as
// R10Unorm_PACKED reads them (green and blue 0)
TEST_F(DevicePackedLevels, TheImageIsAChainOfEveryLevelUnderOneHeader)
{
    gm20b::Texture t;
    ASSERT_TRUE(dev.createYcbcrTexture(0, base, page, nullptr, plan(3), 32, 16, t));
    EXPECT_TRUE(t.levelZero);
    ASSERT_NE(t.chain, nullptr);
    EXPECT_EQ(t.chain->layout.levels, 3u);
    EXPECT_EQ(t.addressMap, 0);
    const auto w = header(t);
    EXPECT_EQ(field(w, 127, 124), 2u) << "MAX_MIP_LEVEL";
    EXPECT_EQ(minLevel(w), 0u);
    EXPECT_EQ(maxLevel(w), 2u);
    EXPECT_EQ(field(w, 143, 128), 31u) << "WIDTH_MINUS_ONE";
    EXPECT_EQ(field(w, 175, 160), 15u) << "HEIGHT_MINUS_ONE";
    EXPECT_EQ(w[0] >> 22 & 63, 0u) << "Y_SOURCE and Z_SOURCE ZERO";
    dev.destroyTexture(t);
}

// Its conversion pass renders the chain's level 0 as its colour target, as it renders a one-level image
// (TegraGM20BMetalRangeConversions.mm makeImages)
TEST_F(DevicePackedLevels, TheConversionPassRendersLevelZeroOfTheChain)
{
    gm20b::Texture t;
    ASSERT_TRUE(dev.createYcbcrTexture(0, base, page, nullptr, plan(3), 32, 16, t));
    const size_t first = kernel_model::model.jobs.size();
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    gm20b::PassDesc p;
    p.color[0].texture = &t;
    p.color[0].load = gm20b::ColorAttachment::Clear;
    p.color[0].store = true;
    p.count = 1;
    const char *why = nullptr;
    gm20b::RenderEncoder *e = cb->render(p, &why);
    ASSERT_NE(e, nullptr) << (why ? why : "");
    e->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    const std::vector<uint64_t> targets = pushed(first).targets;
    ASSERT_FALSE(targets.empty());
    EXPECT_EQ(targets[0], t.va);
    dev.destroyTexture(t);
}

// Each later level from level 0's top-left corner of its size, one copy-engine launch a level; a texture of one level has
// none to copy
TEST_F(DevicePackedLevels, EachLevelIsLevelZerosCornerOfItsSize)
{
    gm20b::Texture t, one;
    ASSERT_TRUE(dev.createYcbcrTexture(0, base, page, nullptr, plan(3), 32, 16, t));
    ASSERT_TRUE(dev.createYcbcrTexture(0, base, page, nullptr, plan(1), 32, 16, one));
    EXPECT_FALSE(one.levelZero);
    EXPECT_EQ(one.chain, nullptr);
    gm20b::Texture level1, level2;
    ASSERT_TRUE(gm20b::chainLevelImage(t, 1, level1) && gm20b::chainLevelImage(t, 2, level2));
    const size_t first = kernel_model::model.jobs.size();
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    gm20b::BlitEncoder *b = cb->blit();
    gm20b::copyLevelZero(*b, t);
    gm20b::copyLevelZero(*b, one);
    b->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    const std::vector<Launch> l = pushed(first).copies;
    ASSERT_EQ(l.size(), 2u);
    EXPECT_EQ(l[0].from, t.va) << "level 0";
    EXPECT_EQ(l[0].to, level1.va);
    EXPECT_EQ(l[0].length, 16u);
    EXPECT_EQ(l[1].from, t.va);
    EXPECT_EQ(l[1].to, level2.va);
    EXPECT_EQ(l[1].length, 8u);
    dev.destroyTexture(one);
    dev.destroyTexture(t);
}

}  // namespace
