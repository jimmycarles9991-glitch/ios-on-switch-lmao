// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchScanReturn.h: the pages Core Animation has back as window A's retirements give them, and the debug kernels'
// check of them wired where NativeLegacyDisplay takes, retires and latches frames.
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"
#include "../SwitchScanReturn.h"

namespace {

using switch_display::ScanReturn;
using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::SourceText;

const char pageA = 0, pageB = 0, pageC = 0;

TEST(ScanReturn, ARetirementGivesBackThePageTheSwapBeforeShowed)
{
    ScanReturn r;
    r.taken(1, &pageA);
    r.taken(2, &pageB);
    EXPECT_EQ(r.retired(1), nullptr);
    EXPECT_FALSE(r.back(&pageA));
    r.taken(3, &pageC);
    EXPECT_EQ(r.retired(2), &pageA);
    EXPECT_TRUE(r.back(&pageA));
    EXPECT_EQ(r.retired(3), &pageB);
    EXPECT_TRUE(r.back(&pageB));
    r.taken(4, &pageA);
    EXPECT_FALSE(r.back(&pageA));
    EXPECT_EQ(r.retired(4), &pageC);
    EXPECT_TRUE(r.back(&pageC));
    EXPECT_TRUE(r.back(&pageB));
}

TEST(ScanReturn, ASwapThatLeavesWindowAAloneGivesNothingBack)
{
    ScanReturn r;
    r.taken(1, &pageA);
    r.taken(2, ScanReturn::kUnchanged);
    r.taken(3, &pageB);
    EXPECT_EQ(r.retired(1), nullptr);
    EXPECT_EQ(r.retired(2), nullptr);
    EXPECT_FALSE(r.back(&pageA));
    EXPECT_EQ(r.retired(3), &pageA);
}

TEST(ScanReturn, AnUnknownPageForgetsTheLastSoNothingIsInvented)
{
    ScanReturn r;
    r.taken(1, &pageA);
    r.taken(2, ScanReturn::kUnknown);
    EXPECT_EQ(r.retired(1), nullptr);
    EXPECT_EQ(r.retired(2), nullptr);
    r.taken(3, &pageA);
    EXPECT_EQ(r.retired(3), nullptr);
    EXPECT_FALSE(r.back(&pageA));
    EXPECT_EQ(r.retired(99), nullptr);
}

TEST(ScanReturn, OnlyPagesAreEverBack)
{
    ScanReturn r;
    EXPECT_FALSE(r.back(nullptr));
    EXPECT_FALSE(r.back(ScanReturn::kUnchanged));
}

std::string display()
{
    return SourceText("../NativeLegacyDisplay.cpp");
}

// Every taken swap records its window A page and tells the read-back whether an offer carried it; the page a
// retirement gives back is checked before the swap leaves the taken table; each frame end checks what window A scans,
// after the latch it took was read.
TEST(ScanReturnSource, TheDisplayChecksEveryHandBackAndFrameEnd)
{
    const std::string source = display();
    const CodeText taken = Block(source, "void noteTaken(const OriginalLegacySwapRequest *request, uint64_t kickAt)");
    EXPECT_TRUE(taken.Before("take(request);", "scanReturnTaken(request);"));
    EXPECT_TRUE(taken.Has("readbackTaken();"));
    const CodeText retire = Block(source, "bool retireSwap(const OriginalLegacySwapRequest *request, bool force)");
    EXPECT_TRUE(retire.Before("scanReturnRetired(request);", "untake(request);"));
    const CodeText tick = Block(source, "void vblankTick()");
    EXPECT_TRUE(tick.Before("checkRotation();", "scanReturnFrameEnd();"));
    const CodeText kick = Block(source, "void kickOnce()");
    EXPECT_TRUE(kick.Before("scanReturnKick(request, memory);", "offerDirect(request, surface, memory,"));
    EXPECT_TRUE(kick.Before("scanReturnKick(request, switch_display::ScanReturn::kUnchanged);",
                            "kickPlaneOnly(request, pd, mach_absolute_time());"));
    EXPECT_TRUE(kick.Before("scanReturnBlank(request);", "noteTaken(request, kickAt);"));
}

// Only a frame tegra_gm20b_scanout_copy accepted keeps the read-back's frame before it: a direct frame flipped from the
// kick goes through noteTaken with no offer and makes it forget.
TEST(ScanReturnSource, OnlyAnAcceptedOfferKeepsTheReadbacksFrameBefore)
{
    const std::string source = display();
    const CodeText direct = Block(source, "bool offerDirect(");
    EXPECT_TRUE(direct.Before("readbackOffer();", "noteTaken(request, kickAt);\n        if (stats)"));
    EXPECT_TRUE(direct.Before("presentDirectVisible(request, entry, width, height, kickAt)", "readbackOffer();"));
    const CodeText copy = Block(source, "bool offerGpuCopy(");
    EXPECT_TRUE(copy.Before("readbackOffer();", "noteTaken(request, kickAt);\n        if (!latchRetire"));
    const CodeText visible = Block(SourceText("../SwitchDirectVisibleDisplay.h"), "bool presentDirectVisible(");
    EXPECT_FALSE(visible.Has("readbackOffer"));
    const CodeText bypass = Block(SourceText("../SwitchReadbackShownDisplay.h"), "void readbackTaken()");
    EXPECT_TRUE(bypass.Has("if (!readbackOffered) tegra_gm20b_scanout_bypassed();"));
    const CodeText forget =
        Block(SourceText("../../TegraGM20B/TegraGM20BReadback.cpp"), "void tgm_readback_bypassed()");
    EXPECT_TRUE(forget.Has("gShown = {};"));
    const CodeText locked = Block(SourceText("../../TegraGM20B/TegraGM20BUserClientPresent.cpp"),
                                  "extern \"C\" void tegra_gm20b_scanout_bypassed(void)");
    EXPECT_TRUE(locked.Before("IOLockLock(gPresentLock);", "tgm_readback_bypassed();"));
}

}  // namespace
