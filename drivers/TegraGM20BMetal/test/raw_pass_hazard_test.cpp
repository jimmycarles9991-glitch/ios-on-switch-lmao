// SPDX-License-Identifier: MIT
// raw/runtime/raw_pass_hazard.h: the held cells are boot 21's line, and a pass opens with the pixel barrier only where
// each of its dependencies on the passes since the last drain is one of them.
#include <gtest/gtest.h>

#include <set>
#include <sstream>
#include <string>
#include <tuple>

#include "drivers/TegraGM20BMetal/raw/runtime/raw_pass_hazard.h"

namespace {

using raw::HazardImage;
using raw::HazardLayout;
using raw::HazardShape;
using raw::HazardSize;
using raw::PassChain;
using raw::PassUses;

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

std::string cellName(const raw::HeldCell &c)
{
    static const char *layouts[] = {"bl", "pitch", "comp"}, *shapes[] = {"same", "stride", "flip", "pyramid", "waw"},
                      *sizes[] = {"s", "m", "f"};
    return std::string(c.format == raw::kHazardBgra8 ? "bgra8" : "rgba16f") + "_" + layouts[int(c.layout)] + "_" +
           shapes[int(c.shape)] + "_" + sizes[int(c.size)];
}

TEST(PassHazard, TheHeldCellsAreBoot21sLine)
{
    std::set<std::string> held, line;
    for (const raw::HeldCell &c : raw::kHeldCells)
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
    i.bytes = uint64_t(width) * height * (format == raw::kHazardRgba16f ? 8 : 4);
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
    raw::advanceChain(chain, a, true);
    return chain;
}

TEST(PassHazard, AReadOfASmallBlockLinearTargetIsHeldWhateverItsShape)
{
    const HazardImage t = image(0x10000, raw::kHazardBgra8, 64, 64);
    PassUses b = pass(64, 512);
    b.write(image(0x80000, raw::kHazardBgra8, 64, 512));
    b.sample(t, false);
    EXPECT_TRUE(raw::pixelBarrierHolds(after(t), b));
    const HazardImage half = image(0x10000, raw::kHazardRgba16f, 48, 30);
    b = pass(64, 512);
    b.sample(half, false);
    EXPECT_TRUE(raw::pixelBarrierHolds(after(half), b));
}

TEST(PassHazard, AMediumTargetIsHeldOnlyAsAPyramidLevelOrAWriteOverIt)
{
    for (HazardLayout layout : {HazardLayout::BlockLinear, HazardLayout::Pitch, HazardLayout::Compressed}) {
        const HazardImage t = image(0x10000, raw::kHazardBgra8, 320, 180, layout);
        PassUses level = pass(160, 90);
        level.write(image(0x200000, raw::kHazardBgra8, 160, 90));
        level.sample(t, false);
        EXPECT_TRUE(raw::pixelBarrierHolds(after(t), level)) << int(layout);
        PassUses copy = pass(320, 180);
        copy.write(image(0x200000, raw::kHazardBgra8, 320, 180));
        copy.sample(t, false);
        EXPECT_FALSE(raw::pixelBarrierHolds(after(t), copy)) << "stride and flip were blind at 320x180";
        PassUses over = pass(320, 180);
        HazardImage view = t;
        view.va += 64;
        view.bytes -= 64;
        over.write(view);
        EXPECT_EQ(raw::pixelBarrierHolds(after(t), over), layout == HazardLayout::BlockLinear) << int(layout);
    }
}

TEST(PassHazard, FullSizeDependenciesKeepTheDrain)
{
    const HazardImage t = image(0x10000, raw::kHazardBgra8, 1280, 720);
    PassUses level = pass(640, 360);
    level.write(image(0x800000, raw::kHazardBgra8, 640, 360));
    level.sample(t, false);
    EXPECT_FALSE(raw::pixelBarrierHolds(after(t), level));
    PassUses unrelated = pass(1280, 720);
    unrelated.write(image(0x900000, raw::kHazardBgra8, 1280, 720));
    EXPECT_TRUE(raw::pixelBarrierHolds(after(t), unrelated)) << "nothing it reads or writes was the last pass's";
}

TEST(PassHazard, WhatNoCellDidKeepsTheDrain)
{
    const HazardImage t = image(0x10000, raw::kHazardBgra8, 64, 64);
    const auto reads = [&](PassUses b) {
        b.sample(t, false);
        return raw::pixelBarrierHolds(after(t), b);
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
    EXPECT_FALSE(raw::pixelBarrierHolds(after(t), b)) << "a named use's stage is not said";
    HazardImage other = image(0x10000, 90, 64, 64);
    b = pass(64, 64);
    b.sample(other, false);
    EXPECT_FALSE(raw::pixelBarrierHolds(after(other), b)) << "RGB10A2 was never probed";
    HazardImage layered = t;
    layered.plain = false;
    b = pass(64, 64);
    b.sample(layered, false);
    EXPECT_FALSE(raw::pixelBarrierHolds(after(layered), b));
    PassChain unknown = after(t);
    unknown.valid = false;
    EXPECT_FALSE(raw::pixelBarrierHolds(unknown, pass(64, 64)));
}

TEST(PassHazard, ATargetAnEarlierPassSampledKeepsTheDrain)
{
    const HazardImage t = image(0x10000, raw::kHazardBgra8, 64, 64), u = image(0x20000, raw::kHazardBgra8, 64, 64);
    PassUses a = pass(64, 64);
    a.write(u);
    a.sample(t, false);
    PassChain chain;
    raw::advanceChain(chain, a, true);
    PassUses b = pass(64, 64);
    b.write(t);
    EXPECT_FALSE(raw::pixelBarrierHolds(chain, b));
}

// Each pass is checked against every pass since the last drain, not only the one before it
TEST(PassHazard, TheChainHoldsEveryPassSinceTheDrain)
{
    const HazardImage big = image(0x10000, raw::kHazardBgra8, 1280, 720),
                      small = image(0x900000, raw::kHazardBgra8, 32, 32);
    PassChain chain = after(big);
    PassUses unrelated = pass(32, 32);
    unrelated.write(small);
    ASSERT_TRUE(raw::pixelBarrierHolds(chain, unrelated));
    raw::advanceChain(chain, unrelated, false);
    PassUses reader = pass(640, 360);
    reader.write(image(0xa00000, raw::kHazardBgra8, 640, 360));
    reader.sample(big, false);
    EXPECT_FALSE(raw::pixelBarrierHolds(chain, reader)) << "the 1280x720 target two passes back";
    raw::advanceChain(chain, reader, true);
    EXPECT_EQ(chain.writtenCount, 1u) << "a drain leaves the drained pass alone";
}

}  // namespace
