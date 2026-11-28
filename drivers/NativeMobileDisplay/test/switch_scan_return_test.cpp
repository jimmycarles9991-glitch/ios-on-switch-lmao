// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchScanReturn.h: the pages Core Animation has back as window A's retirements give them, and the debug kernels'
// check of them wired where NativeLegacyDisplay takes, retires and latches frames.
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"
#include "../SwitchColourBlank.h"
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
    EXPECT_TRUE(taken.before("take(request);", "scanReturnTaken(request);"));
    EXPECT_TRUE(taken.has("readbackTaken();"));
    const CodeText retire = Block(source, "bool retireSwap(const OriginalLegacySwapRequest *request, bool force)");
    EXPECT_TRUE(retire.before("scanReturnRetired(request);", "untake(request);"));
    const CodeText tick = Block(source, "void vblankTick()");
    EXPECT_TRUE(tick.before("checkRotation();", "scanReturnFrameEnd();"));
    const CodeText kick = Block(source, "void kickOnce()");
    EXPECT_TRUE(kick.before("scanReturnKick(request, memory);", "offerDirect(request, surface, memory,"));
    EXPECT_TRUE(kick.before("scanReturnKick(request, switch_display::ScanReturn::kUnchanged);",
                            "kickPlaneOnly(request, pd, mach_absolute_time());"));
    EXPECT_TRUE(kick.before("scanReturnBlank(request);", "noteTaken(request, kickAt);"));
}

// Only a frame tegra_gm20b_scanout_copy accepted keeps the read-back's frame before it: a direct frame flipped from the
// kick goes through noteTaken with no offer and makes it forget.
TEST(ScanReturnSource, OnlyAnAcceptedOfferKeepsTheReadbacksFrameBefore)
{
    const std::string source = display();
    const CodeText direct = Block(source, "bool offerDirect(");
    EXPECT_TRUE(direct.before("readbackOffer();", "noteTaken(request, kickAt);\n        if (stats)"));
    EXPECT_TRUE(direct.before("presentDirectVisible(request, entry, width, height, kickAt)", "readbackOffer();"));
    const CodeText copy = Block(source, "bool offerGpuCopy(");
    EXPECT_TRUE(copy.before("readbackOffer();", "noteTaken(request, kickAt);\n        if (!latchRetire"));
    const CodeText visible = Block(SourceText("../SwitchDirectVisibleDisplay.h"), "bool presentDirectVisible(");
    EXPECT_FALSE(visible.has("readbackOffer"));
    const CodeText bypass = Block(SourceText("../SwitchReadbackShownDisplay.h"), "void readbackTaken()");
    EXPECT_TRUE(bypass.has("if (!readbackOffered) tegra_gm20b_scanout_bypassed();"));
    const CodeText forget =
        Block(SourceText("../../TegraGM20B/TegraGM20BReadback.cpp"), "void tgm_readback_bypassed()");
    EXPECT_TRUE(forget.has("gShown = {};"));
    const CodeText locked = Block(SourceText("../../TegraGM20B/TegraGM20BUserClientPresent.cpp"),
                                  "extern \"C\" void tegra_gm20b_scanout_bypassed(void)");
    EXPECT_TRUE(locked.before("IOLockLock(gPresentLock);", "tgm_readback_bypassed();"));
}

// A colour blank swap retires once its black is on screen: one frame end after the one at which the colour unit
// settled its generation, since that action's CMU_ENABLE latches at the next frame start. Until then window A shows the
// page before it, which the retirement gives back.
TEST(ColourBlank, TheBlankSwapRetiresAFrameAfterTheColourUnitSettledIt)
{
    switch_display::ColourBlank blank;
    blank.kicked(7, 40);
    EXPECT_TRUE(blank.holds(7));
    EXPECT_FALSE(blank.holds(6));
    EXPECT_EQ(blank.generation(), 40u);
    EXPECT_EQ(blank.frameEnd(100, false), 0u);
    EXPECT_FALSE(blank.hidesWindowA());
    EXPECT_EQ(blank.frameEnd(101, true), 0u) << "settled at this frame end: the enable written now latches next";
    EXPECT_FALSE(blank.hidesWindowA());
    EXPECT_EQ(blank.frameEnd(102, true), 7u);
    EXPECT_TRUE(blank.hidesWindowA());
    EXPECT_FALSE(blank.waiting());
    EXPECT_EQ(blank.frameEnd(103, true), 0u);
    EXPECT_EQ(blank.ended(), 0u);
    EXPECT_FALSE(blank.hidesWindowA());
}

// A flip that latches, or a stop, before the blank is shown takes the waiting blank swap with it; a second blank swap
// while the first is shown keeps window A hidden.
TEST(ColourBlank, AFlipOrAStopTakesTheWaitingBlankSwapAndASecondBlankStaysHidden)
{
    switch_display::ColourBlank blank;
    blank.kicked(3, 1);
    EXPECT_EQ(blank.ended(), 3u);
    EXPECT_FALSE(blank.waiting());
    blank.kicked(4, 2);
    blank.frameEnd(10, true);
    EXPECT_EQ(blank.frameEnd(11, true), 4u);
    blank.kicked(5, 2);
    EXPECT_TRUE(blank.hidesWindowA()) << "the black is still on screen";
    EXPECT_TRUE(blank.waiting());
}

// The colour blank's wiring: the swap is held from its kick's retirement, retired by the frame end after the colour unit's
// action, keeps frame ends coming meanwhile, and goes with the next latch or a stop; the scan check takes window A as
// showing nothing of the page under a shown blank.
TEST(ScanReturnSource, AColourBlankSwapRetiresOnceItsBlackIsOnScreen)
{
    const std::string source = display();
    const CodeText kick = Block(source, "void kickOnce()");
    EXPECT_TRUE(
        kick.before("noteTaken(request, kickAt);", "colourBlankSwap.kicked(seqOf(request), color->generation());"));
    const CodeText finish = Block(source, "void finishTransfer(const OriginalLegacySwapRequest *request, bool force");
    EXPECT_TRUE(finish.before("!colourBlankSwap.holds(seqOf(request))", "retireThrough(seqOf(request),"));
    const CodeText tick = Block(source, "void vblankTick()");
    EXPECT_TRUE(tick.before("color->frameEndAction(mayRequest);", "colourBlankFrameEnd();"));
    const CodeText frameEnd = Block(source, "void colourBlankFrameEnd()");
    EXPECT_TRUE(frameEnd.has("color->settledThrough(colourBlankSwap.generation())"));
    EXPECT_TRUE(frameEnd.has("retireThrough(seq,"));
    EXPECT_TRUE(Block(source, "switch_frame_end::Holder frameEndHolder()").has("colourBlankSwap.waiting()"));
    EXPECT_TRUE(Block(source, "void kickSwapQueue() override").before("kickUnmaskFrameEnd();", "kickOnce();"))
        << "a blank kicked under a blank already shown publishes no colour change: the kick itself routes FRAME_END";
    EXPECT_TRUE(Block(source, "void flipLatched()").has("colourBlankSwap.ended();"));
    const CodeText stopped = Block(source, "void settleStopped()");
    EXPECT_TRUE(stopped.before("colourBlankSwap.ended();", "retireThrough(seq, mach_absolute_time());"));
    const CodeText suspend = Block(source, "void quiesceForSuspend()");
    EXPECT_TRUE(suspend.before("colourBlankSwap.ended();", "retireSuspendShown(shown > blank ? shown : blank);"));
    EXPECT_TRUE(kick.has("if (colourBlank && latchRetire && !scanoutStopped && !hwSuspended)"))
        << "a blank kicked with no frame end to come is on screen at once, as before";
    const std::string check = SourceText("../SwitchScanReturnDisplay.h");
    const CodeText reach = Block(check, "const char *scanReach(int entry)");
    EXPECT_TRUE(reach.has("const bool hidden = colourBlankSwap.hidesWindowA();"));
    EXPECT_TRUE(reach.has("if (!hidden && entry == ring.front)"));
    EXPECT_TRUE(Block(check, "void scanReturnFrameEnd()").has("!colourBlankSwap.hidesWindowA()"));
}

}  // namespace
