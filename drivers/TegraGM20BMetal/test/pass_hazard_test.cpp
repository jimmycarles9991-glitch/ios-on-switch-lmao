// SPDX-License-Identifier: MIT
// pass_hazard.h: the held cells are boot 21's line, and a pass opens with the pixel barrier only where
// each of its dependencies on the passes since the last drain is one of them; a memoryless attachment's backing, scratch,
// is no dependency on another pass's scratch over the same memory.
#include <gtest/gtest.h>

#include <set>
#include <sstream>
#include <string>
#include <tuple>

#include "drivers/TegraGM20BMetal/pass_hazard.h"

namespace {

using gm20b::HazardImage;
using gm20b::HazardLayout;
using gm20b::HazardShape;
using gm20b::HazardSize;
using gm20b::PassChain;
using gm20b::PassUses;

// gpu.x1.pass_pixel_barrier pixel_cells, boot 21 (stack-T-v5b, -20261113-134714.json)
constexpr const char *kBoot21Cells =
    "bgra8_bl_same_s:H:1504:0,bgra8_bl_same_m:H:1168:0,bgra8_bl_stride_s:H:1504:0,bgra8_bl_stride_m:B:1504:0,"
    "bgra8_bl_flip_s:H:1504:0,bgra8_bl_flip_m:B:896:0,bgra8_bl_pyramid_s:H:1504:0,bgra8_bl_pyramid_m:H:992:0,"
    "bgra8_bl_stride_f:B:600:0,bgra8_bl_pyramid_f:B:328:0,bgra8_pitch_same_m:H:1176:0,bgra8_pitch_pyramid_m:H:992:0,"
    "bgra8_comp_same_m:H:1176:0,bgra8_comp_pyramid_m:H:992:0,bgra8_bl_waw_m:H:1112:0,rgba16f_bl_same_s:H:1504:0,"
    "rgba16f_bl_same_m:H:1008:0,rgba16f_bl_stride_s:H:1504:0,rgba16f_bl_stride_m:B:1504:0,rgba16f_bl_flip_s:H:1504:0,"
    "rgba16f_bl_flip_m:B:768:0,rgba16f_bl_pyramid_s:H:1504:0,rgba16f_bl_pyramid_m:H:872:0,"
    "rgba16f_bl_stride_f:B:600:0,rgba16f_bl_pyramid_f:B:312:0,rgba16f_pitch_same_m:H:1008:0,"
    "rgba16f_pitch_pyramid_m:H:872:0,rgba16f_comp_same_m:H:1008:0,rgba16f_comp_pyramid_m:H:872:0,"
    "rgba16f_bl_waw_m:H:992:0";

std::string cellName(const gm20b::HeldCell &c)
{
    static const char *layouts[] = {"bl", "pitch", "comp"}, *shapes[] = {"same", "stride", "flip", "pyramid", "waw"},
                      *sizes[] = {"s", "m", "f"};
    return std::string(c.format == gm20b::kHazardBgra8 ? "bgra8" : "rgba16f") + "_" + layouts[int(c.layout)] + "_" +
           shapes[int(c.shape)] + "_" + sizes[int(c.size)];
}

TEST(PassHazard, TheHeldCellsAreBoot21sLine)
{
    std::set<std::string> held, line;
    for (const gm20b::HeldCell &c : gm20b::kHeldCells)
        held.insert(cellName(c));
    std::stringstream cells(kBoot21Cells);
    for (std::string cell; std::getline(cells, cell, ',');)
        if (cell.find(":H:") != std::string::npos)
            line.insert(cell.substr(0, cell.find(':')));
    EXPECT_EQ(held, line);
    EXPECT_EQ(held.size(), 22u);
}

HazardImage image(uint64_t va, uint32_t format, uint32_t width, uint32_t height,
                  HazardLayout layout = HazardLayout::BlockLinear)
{
    HazardImage i;
    i.va = va;
    i.bytes = uint64_t(width) * height * (format == gm20b::kHazardRgba16f ? 8 : 4);
    i.format = format;
    i.width = width;
    i.height = height;
    i.layout = layout;
    i.plain = true;
    return i;
}

PassUses pass(uint32_t width, uint32_t height)
{
    PassUses p;
    p.known = true;
    p.areaWidth = width;
    p.areaHeight = height;
    return p;
}

// The chain after one pass writing `written` (drained before it)
PassChain after(const HazardImage &written)
{
    PassUses a = pass(written.width, written.height);
    a.write(written);
    PassChain chain;
    gm20b::advanceChain(chain, a, true);
    return chain;
}

TEST(PassHazard, AReadOfASmallBlockLinearTargetIsHeldWhateverItsShape)
{
    const HazardImage t = image(0x10000, gm20b::kHazardBgra8, 64, 64);
    PassUses b = pass(64, 512);
    b.write(image(0x80000, gm20b::kHazardBgra8, 64, 512));
    b.sample(t, false);
    EXPECT_TRUE(gm20b::pixelBarrierHolds(after(t), b));
    const HazardImage half = image(0x10000, gm20b::kHazardRgba16f, 48, 30);
    b = pass(64, 512);
    b.sample(half, false);
    EXPECT_TRUE(gm20b::pixelBarrierHolds(after(half), b));
}

TEST(PassHazard, AMediumTargetIsHeldOnlyAsAPyramidLevelOrAWriteOverIt)
{
    for (HazardLayout layout : {HazardLayout::BlockLinear, HazardLayout::Pitch, HazardLayout::Compressed}) {
        const HazardImage t = image(0x10000, gm20b::kHazardBgra8, 320, 180, layout);
        PassUses level = pass(160, 90);
        level.write(image(0x200000, gm20b::kHazardBgra8, 160, 90));
        level.sample(t, false);
        EXPECT_TRUE(gm20b::pixelBarrierHolds(after(t), level)) << int(layout);
        PassUses copy = pass(320, 180);
        copy.write(image(0x200000, gm20b::kHazardBgra8, 320, 180));
        copy.sample(t, false);
        EXPECT_FALSE(gm20b::pixelBarrierHolds(after(t), copy)) << "stride and flip were blind at 320x180";
        PassUses over = pass(320, 180);
        HazardImage view = t;
        view.va += 64;
        view.bytes -= 64;
        over.write(view);
        EXPECT_EQ(gm20b::pixelBarrierHolds(after(t), over), layout == HazardLayout::BlockLinear) << int(layout);
    }
}

TEST(PassHazard, FullSizeDependenciesKeepTheDrain)
{
    const HazardImage t = image(0x10000, gm20b::kHazardBgra8, 1280, 720);
    PassUses level = pass(640, 360);
    level.write(image(0x800000, gm20b::kHazardBgra8, 640, 360));
    level.sample(t, false);
    EXPECT_FALSE(gm20b::pixelBarrierHolds(after(t), level));
    PassUses unrelated = pass(1280, 720);
    unrelated.write(image(0x900000, gm20b::kHazardBgra8, 1280, 720));
    EXPECT_TRUE(gm20b::pixelBarrierHolds(after(t), unrelated)) << "nothing it reads or writes was the last pass's";
}

TEST(PassHazard, WhatNoCellDidKeepsTheDrain)
{
    const HazardImage t = image(0x10000, gm20b::kHazardBgra8, 64, 64);
    const auto reads = [&](PassUses b) {
        b.sample(t, false);
        return gm20b::pixelBarrierHolds(after(t), b);
    };
    PassUses b = pass(64, 64);
    b.known = false;
    EXPECT_FALSE(reads(b)) << "a heap, a fence wait or a storage write";
    b = pass(64, 64);
    b.vertexSamples = true;
    EXPECT_FALSE(reads(b));
    b = pass(64, 64);
    b.depthStencil = true;
    EXPECT_FALSE(reads(b));
    b = pass(64, 64);
    b.sample(t, true);
    EXPECT_FALSE(gm20b::pixelBarrierHolds(after(t), b)) << "a named use's stage is not said";
    HazardImage other = image(0x10000, 90, 64, 64);
    b = pass(64, 64);
    b.sample(other, false);
    EXPECT_FALSE(gm20b::pixelBarrierHolds(after(other), b)) << "RGB10A2 was never probed";
    HazardImage layered = t;
    layered.plain = false;
    b = pass(64, 64);
    b.sample(layered, false);
    EXPECT_FALSE(gm20b::pixelBarrierHolds(after(layered), b));
    PassChain unknown = after(t);
    unknown.valid = false;
    EXPECT_FALSE(gm20b::pixelBarrierHolds(unknown, pass(64, 64)));
}

TEST(PassHazard, ATargetAnEarlierPassSampledKeepsTheDrain)
{
    const HazardImage t = image(0x10000, gm20b::kHazardBgra8, 64, 64), u = image(0x20000, gm20b::kHazardBgra8, 64, 64);
    PassUses a = pass(64, 64);
    a.write(u);
    a.sample(t, false);
    PassChain chain;
    gm20b::advanceChain(chain, a, true);
    PassUses b = pass(64, 64);
    b.write(t);
    EXPECT_FALSE(gm20b::pixelBarrierHolds(chain, b));
}

// Each pass is checked against every pass since the last drain, not only the one before it
TEST(PassHazard, TheChainHoldsEveryPassSinceTheDrain)
{
    const HazardImage big = image(0x10000, gm20b::kHazardBgra8, 1280, 720),
                      small = image(0x900000, gm20b::kHazardBgra8, 32, 32);
    PassChain chain = after(big);
    PassUses unrelated = pass(32, 32);
    unrelated.write(small);
    ASSERT_TRUE(gm20b::pixelBarrierHolds(chain, unrelated));
    gm20b::advanceChain(chain, unrelated, false);
    PassUses reader = pass(640, 360);
    reader.write(image(0xa00000, gm20b::kHazardBgra8, 640, 360));
    reader.sample(big, false);
    EXPECT_FALSE(gm20b::pixelBarrierHolds(chain, reader)) << "the 1280x720 target two passes back";
    gm20b::advanceChain(chain, reader, true);
    EXPECT_EQ(chain.writtenCount, 1u) << "a drain leaves the drained pass alone";
}

// A memoryless attachment's backing holds nothing a later pass may read (its contents are undefined at each pass's start):
// passes whose scratch shares that memory depend on nothing through it, whatever its size
TEST(PassHazard, ScratchOverTheSameBackingIsNoDependency)
{
    const HazardImage backing = image(0x4000000, gm20b::kHazardRgba16f, 768, 1344);
    PassUses a = pass(722, 1282);
    a.write(image(0x10000, gm20b::kHazardBgra8, 722, 1282));
    a.scratchWrite(backing);
    PassChain chain;
    gm20b::advanceChain(chain, a, true);
    PassUses b = pass(1792, 1472);
    b.write(image(0x2000000, gm20b::kHazardBgra8, 1792, 1472));
    HazardImage larger = backing;
    larger.bytes *= 3;
    b.scratchWrite(larger);
    EXPECT_EQ(gm20b::drainCause(chain, b), gm20b::DrainCause::None);
}

// Scratch over memory a real image of the chain names, or a real image over an earlier pass's scratch, keeps the drain
TEST(PassHazard, ScratchOverARealImageKeepsTheDrain)
{
    const HazardImage target = image(0x10000, gm20b::kHazardBgra8, 320, 180);
    PassUses a = pass(320, 180);
    a.write(target);
    PassChain chain;
    gm20b::advanceChain(chain, a, true);
    PassUses b = pass(320, 180);
    b.scratchWrite(target);
    EXPECT_EQ(gm20b::drainCause(chain, b), gm20b::DrainCause::Scratch);
    PassUses c = pass(320, 180);
    c.scratchWrite(image(0x900000, gm20b::kHazardRgba16f, 320, 180));
    gm20b::advanceChain(chain, c, true);
    PassUses d = pass(320, 180);
    d.sample(image(0x900000, gm20b::kHazardRgba16f, 320, 180), false);
    EXPECT_EQ(gm20b::drainCause(chain, d), gm20b::DrainCause::Scratch);
    PassUses e = pass(320, 180);
    e.write(image(0x900000, gm20b::kHazardRgba16f, 320, 180));
    EXPECT_EQ(gm20b::drainCause(chain, e), gm20b::DrainCause::Scratch);
}

// The same textures sampled by every pass are listed once: forty passes of four textures of a set of five stay one chain
TEST(PassHazard, TheChainListsEachImageOnce)
{
    PassChain chain;
    for (uint32_t p = 0; p < 40; p++) {
        PassUses next = pass(722, 1282);
        next.write(image(0x10000000 + uint64_t(p) * 0x1000000, gm20b::kHazardBgra8, 722, 1282));
        for (uint32_t t = 0; t < 4; t++)
            next.sample(image(0x100000 + uint64_t((p + t) % 5) * 0x10000, gm20b::kHazardBgra8, 64, 64), false);
        ASSERT_TRUE(p == 0 || gm20b::pixelBarrierHolds(chain, next)) << p;
        gm20b::advanceChain(chain, next, p == 0);
    }
    EXPECT_TRUE(chain.valid);
    EXPECT_EQ(chain.sampledCount, 5u);
    EXPECT_EQ(chain.writtenCount, 40u);
}

TEST(PassHazard, EachDrainSaysWhy)
{
    const HazardImage big = image(0x10000, gm20b::kHazardBgra8, 1280, 720);
    PassUses unknown = pass(64, 64);
    unknown.known = false;
    EXPECT_EQ(gm20b::drainCause(after(big), unknown), gm20b::DrainCause::Unknown);
    PassChain invalid = after(big);
    invalid.valid = false;
    EXPECT_EQ(gm20b::drainCause(invalid, pass(64, 64)), gm20b::DrainCause::Chain);
    PassUses reader = pass(640, 360);
    reader.sample(big, false);
    EXPECT_EQ(gm20b::drainCause(after(big), reader), gm20b::DrainCause::Read);
    PassUses over = pass(1280, 720);
    over.write(big);
    EXPECT_EQ(gm20b::drainCause(after(big), over), gm20b::DrainCause::Write);
    PassUses depth = pass(64, 64);
    depth.depthStencil = true;
    EXPECT_EQ(gm20b::drainCause(after(big), depth), gm20b::DrainCause::Depth);
}

}  // namespace
