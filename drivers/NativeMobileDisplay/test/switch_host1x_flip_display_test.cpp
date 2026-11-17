// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchHost1xFlipDisplay.h: debug kernels only, on the display's own host1x channel (claimed as the class probe
// claims it), flipping only to an alias every page of which is the front's, driven at the frame end before
// checkRotation reads the underflow. While it runs nothing else of the display writes window A or a request: the
// swaps are held, completion and media flips are not armed, no change of mode goes out, the colour and palette units
// ask for nothing, and no other experiment starts. It ends at every way the display stops.
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

namespace {

using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::SourceText;

TEST(Host1xFlipDisplay, ItIsDebugOnlyOnTheDisplaysOwnChannel)
{
    const std::string trial = SourceText("../SwitchHost1xFlipDisplay.h");
    const size_t gate = trial.find("#if SWITCH_FRAME_TIMELINE");
    ASSERT_NE(gate, std::string::npos);
    EXPECT_LT(gate, trial.find("int host1xFlipStart("));
    EXPECT_LT(trial.find("#else"), trial.rfind("void host1xFlipAbort(const char *)\n{}"));
    const CodeText code(trial);
    EXPECT_TRUE(code.has("kHost1xFlipChannel = TEGRA_HOST1X_CHANNEL_DISPLAY"));
    EXPECT_FALSE(code.has("TEGRA_HOST1X_CHANNEL_NVJPG"));
    EXPECT_EQ(code.count("__atomic_exchange_n(&dcClassBusy, true, __ATOMIC_ACQUIRE)"), 1u);
    EXPECT_TRUE(
        Block(trial, "void host1xFlipFinished()").has("__atomic_store_n(&dcClassBusy, false, __ATOMIC_RELEASE);"));
    EXPECT_TRUE(Block(trial, "switch_host1x_flip::Active host1xFlipActive()")
                    .before("seen.control = dc[kDcStateControl];", "windowActive(kWinbufStartAddr)"));
    EXPECT_TRUE(
        Block(trial, "void host1xFlipFinished()").before("planeDropSlot(", "native_legacy_frame_end_release();"));
    EXPECT_EQ(code.count("host1xFlipsLatched + 1"), 1u);
    EXPECT_TRUE(Block(trial, "const char *host1xFlipSurfaces(")
                    .before("if (!host1xFlipAliasIsFront(entry, k))", "host1xFlip.aliasSlot = k;"));
    EXPECT_TRUE(Block(trial, "bool host1xFlipAliasIsFront(int entry, int k) const").has("if (mapped != scanned)"));
    EXPECT_TRUE(Block(trial, "void host1xFlipFinished()").has("!planeSlotBusy(s.aliasSlot)"));

    const std::string probe = SourceText("../SwitchDcHost1xClassProbe.h");
    EXPECT_NE(probe.find("kDcClassChannel = TEGRA_HOST1X_CHANNEL_DISPLAY"), std::string::npos);
}

TEST(Host1xFlipDisplay, NothingElseWritesWindowAOrARequestWhileItRuns)
{
    const std::string display = SourceText("../NativeLegacyDisplay.cpp");
    EXPECT_NE(display.find("#include \"SwitchDcHost1xClassProbe.h\"\n#include \"SwitchHost1xFlipDisplay.h\""),
              std::string::npos);
    EXPECT_NE(display.find("            dcClassRegister();\n            host1xFlipRegister();"), std::string::npos);
    const CodeText tick = Block(display, "void vblankTick()");
    EXPECT_TRUE(tick.before("underflowRead();", "host1xFlipFrameEnd();"));
    EXPECT_TRUE(tick.before("host1xFlipFrameEnd();", "checkRotation();"));
    EXPECT_TRUE(tick.has("!columnScanBusy() && !windowProbeHolds() && !host1xFlipBusy();"));
    EXPECT_TRUE(
        Block(display, "void kickOnce()").has("if (columnScanBusy() || windowProbeHolds() || host1xFlipBusy()) {"));
    for (const char *stop : {"void stopRotation()", "bool setScanout(bool on)", "void quiesceForSuspend()"})
        EXPECT_TRUE(Block(display, stop).has("host1xFlipAbort(\"")) << stop;

    EXPECT_TRUE(
        Block(SourceText("../SwitchCompletionFlipDisplay.h"), "bool completionFlipMay(").has("host1xFlipBusy()"));
    EXPECT_TRUE(
        Block(SourceText("../SwitchMediaCompletionFlipDisplay.h"), "bool mediaFlipMay(").has("host1xFlipBusy()"));
    EXPECT_TRUE(
        Block(SourceText("../SwitchLandscapeDisplay.h"), "const char *landscapeBusy()").has("host1xFlipBusy()"));
    EXPECT_TRUE(Block(SourceText("../NativeColumnScanDisplay.h"), "int columnScanTrigger(").has("!host1xFlipBusy()"));
    EXPECT_TRUE(
        Block(SourceText("../DisplayWindowProbeDisplay.h"), "IOReturn windowProbeCall(").has("host1xFlipBusy()"));
}

}  // namespace
