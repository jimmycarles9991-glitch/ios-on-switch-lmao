// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchFrameEndHolder.h: the frame ends a release kernel would also have routed leave out the debug-only holders, and
// every holder has a name for the locks line.
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "../SwitchFrameEndHolder.h"
#include "bazel/testing/source_text.h"

using namespace switch_frame_end;
using switch_ios::test::Block;
using switch_ios::test::SourceText;

namespace {

// 25 Nov boot 2693043C: the boot window held by the console tape and landscape_video's sample hold are debug-only.
TEST(FrameEndHolder, DebugOnlyHoldersAreNotCountedAsRelease)
{
    RoutedCounts counts;
    for (int i = 0; i < 300; ++i)
        counts.add(kTape);
    for (int i = 0; i < 295; ++i)
        counts.add(kExperiment);
    counts.add(kProbe);
    EXPECT_EQ(counts.release(), 0u);
    counts.add(kHold);
    counts.add(kKick);
    counts.add(kPanel);
    EXPECT_EQ(counts.release(), 3u);
}

TEST(FrameEndHolder, RoutedCountsEveryHolderButNone)
{
    RoutedCounts counts;
    counts.add(kNone);
    counts.add(kTape);
    counts.add(kRing);
    EXPECT_EQ(counts.routed(), 2u);
    EXPECT_EQ(counts.routed() - counts.release(), 1u);
}

TEST(FrameEndHolder, NoneIsNeverARelease)
{
    RoutedCounts counts;
    counts.add(kNone);
    counts.add(static_cast<Holder>(kHolders + 3));
    EXPECT_EQ(counts.by[kNone], 2u);
    EXPECT_EQ(counts.release(), 0u);
}

TEST(FrameEndHolder, EveryHolderHasOneWordName)
{
    for (unsigned h = 0; h < kHolders; ++h) {
        ASSERT_NE(kHolderNames[h], nullptr) << h;
        EXPECT_GT(std::strlen(kHolderNames[h]), 0u) << h;
        EXPECT_EQ(std::strchr(kHolderNames[h], ' '), nullptr) << kHolderNames[h];
    }
}

// The routed note runs after the idle mask: it reads whether FRAME_END stayed unmasked, and a debug experiment or a
// frame-end hold is named only once the display's own reasons are none.
TEST(FrameEndHolderSource, TheNoteNamesTheDisplaysReasonFirst)
{
    const std::string display = SourceText("../NativeLegacyDisplay.cpp");
    const auto note = Block(display, "void noteFrameEndRouted()");
    EXPECT_TRUE(note.before("if (!__atomic_load_n(&frameEndUnmasked", "fe::Holder holder = experiment"));
    EXPECT_TRUE(note.before("holder = fe::kHold;", "holder = fe::kPanel;"));
    // The debug-only holders are asked last: a release holder they overlap is the release kernel's.
    const auto holder = Block(display, "switch_frame_end::Holder frameEndHolder()");
    EXPECT_TRUE(holder.before("return fe::kBlank;", "return fe::kTape;"));
    EXPECT_TRUE(holder.before("return fe::kTape;", "return fe::kProbe;"));
    EXPECT_TRUE(holder.before("return fe::kProbe;", "return fe::kNone;"));
    EXPECT_TRUE(Block(display, "bool frameEndNeeded()").has("return frameEndHolder() != switch_frame_end::kNone;"));
}

}  // namespace
