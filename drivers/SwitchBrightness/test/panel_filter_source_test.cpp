// SPDX-License-Identifier: BSD-2-Clause-Patent
// tegra_panel_frame_end_filter runs in the display's DC A filter (primary interrupt context) at every frame end: no lock,
// mutex, sleep, log or allocation, and no read-modify-write on its frame count, which only that filter writes. Its
// step timer is a timer_call (XNU's own spinlock), which a filter may arm.
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::SourceText;

namespace {
const std::string &panel()
{
    static const std::string source = SourceText("../TegraPanel.cpp");
    return source;
}
}  // namespace

TEST(PanelFilterSource, TheFrameEndStepTakesNothingThatBlocksOrSpins)
{
    for (const char *signature : {"extern \"C\" void tegra_panel_frame_end_filter(void)", "void advance(Move move)"}) {
        const CodeText body = Block(panel(), signature);
        ASSERT_TRUE(body.Has("__atomic_")) << signature;
        for (const char *banned : {"IOSimpleLock", "IOLock", "IOMalloc", "IONew", "IOLog", "IOSleep", "thread_block",
                                   "assert_wait", "__atomic_fetch_", "__atomic_exchange", "__atomic_compare_exchange"})
            EXPECT_FALSE(body.Has(banned)) << signature << " has " << banned;
    }
    EXPECT_TRUE(
        Block(panel(), "extern \"C\" void tegra_panel_frame_end_filter(void)")
            .Has("__atomic_store_n(&gFrames, __atomic_load_n(&gFrames, __ATOMIC_RELAXED) + 1, __ATOMIC_RELEASE);"));
}

TEST(PanelFilterSource, OnlyTheFilterWritesTheFrameCount)
{
    const CodeText code(panel());
    EXPECT_EQ(code.Count("&gFrames, __atomic_load_n(&gFrames"), 1u);
    EXPECT_EQ(code.Count("__atomic_store_n(&gFrames"), 1u);
}
