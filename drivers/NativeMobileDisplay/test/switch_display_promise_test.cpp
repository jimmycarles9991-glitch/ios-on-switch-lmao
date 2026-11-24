// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"
#include "switch_display_promise_harness.h"

namespace display_promise_test {
namespace {

using switch_ios::test::SourceText;

class DisplayPromiseTest : public testing::Test {
protected:
    std::unique_ptr<Display> dp;
    PromiseKernel kernel;
    switch_gpu_backing backings[Display::kSlots]{};

    Display &d()
    {
        return *dp;
    }

    void resetHarness()
    {
        if (dp) {
            for (unsigned k = 0; k < Display::kSlots; ++k)
                dp->releaseScanoutBacking(k);
        }
        gNowNs = 1000000000ULL;
        gLog.clear();
        gOffer = nullptr;
        gGpuCallout = nullptr;
        gGpuContext = nullptr;
        gExternalStatus = 1;
        gPanicRestore = {};
        for (auto &b : backings)
            b = {};
        dp = std::make_unique<Display>();
        kernel.reset(dp.get());
        for (unsigned k = 0; k < Display::kSlots; ++k) {
            dp->reads[k].lease = reinterpret_cast<switch_gpu_cpu_lease *>(1);
            dp->reads[k].view.native = &backings[k];
            dp->reads[k].view.producer = {10 + k, 1, SWITCH_GPU_ENGINE_GM20B, 9};
            dp->slots[k].used = 1;
            dp->holdScanoutBacking(k);
            dp->slots[k].used = 2;
        }
    }

    void SetUp() override
    {
        resetHarness();
    }

    void TearDown() override
    {
        gOffer = nullptr;
        if (dp) {
            for (unsigned k = 0; k < Display::kSlots; ++k)
                dp->releaseScanoutBacking(k);
            dp.reset();
        }
    }

    void presentInitialFrame(int slot)
    {
        const auto *req = d().enqueueSwap();
        ASSERT_TRUE(d().offerDirectSwap(slot, req));
        d().boundaryAndTick();
        ASSERT_EQ(d().activeEntry(), slot);
        ASSERT_EQ(d().ring.front, slot);
    }

    size_t logCount(const std::string &substr) const
    {
        return static_cast<size_t>(std::count_if(gLog.begin(), gLog.end(), [&](const std::string &line) {
            return line.find(substr) != std::string::npos;
        }));
    }
};

TEST_F(DisplayPromiseTest, PromisedDirectSwapKeptLateWaitsCleanlyAndPresents)
{
    for (const bool useFilter : {true, false}) {
        SCOPED_TRACE(useFilter ? "filter" : "workloop");
        resetHarness();
        constexpr int kSlot0 = Display::kSlot0, kSlot1 = Display::kSlot0 + 1;
        presentInitialFrame(kSlot0);
        gLog.clear();

        backings[1].promises = 1;
        unsigned offers = 0;
        gOffer = [&](const TegraScanoutCopy *copy) {
            ++offers;
            EXPECT_NE(copy->flags & TEGRA_SCANOUT_COPY_FENCE_ONLY, 0u);
            EXPECT_NE(copy->flags & TEGRA_SCANOUT_COPY_PRODUCER_WATCH, 0u);
            EXPECT_EQ(kernel.parkPromisedOffer(), 0);
            return 0;
        };

        const auto *req1 = d().enqueueSwap();
        ASSERT_TRUE(d().offerDirectSwap(kSlot1, req1));
        EXPECT_EQ(offers, 1u) << "promised surface must not flip directly from kick";
        EXPECT_EQ(switch_gpu_present_watch_state(&kernel.watch), uint32_t(SWITCH_GPU_PRESENT_WATCH_PROMISED));
        EXPECT_EQ(d().activeEntry(), kSlot0);
        EXPECT_EQ(d().ring.pending, Display::kNoBuffer);

        for (int f = 0; f < 5; ++f) {
            gNowNs += 500000000ULL;
            kernel.runGiveUp();
            d().boundaryAndTick();
            d().checkFlipDeadline();
            EXPECT_EQ(d().completionFlip.state(), SwitchCompletionFlip::kArmed);
            EXPECT_EQ(d().activeEntry(), kSlot0);
            EXPECT_EQ(d().ring.front, kSlot0);
            EXPECT_EQ(kernel.steppedRaised, 0u);
        }
        ASSERT_EQ(d().swaps.size(), 1u);

        backings[1].promises = 0;
        ASSERT_EQ(kernel.keepLate(42, useFilter), 1);
        EXPECT_EQ(kernel.lastClaim, SWITCH_GPU_PRESENT_WATCH_VISIBLE);
        d().donePass();
        d().boundaryAndTick();

        EXPECT_EQ(d().activeEntry(), kSlot1);
        EXPECT_EQ(d().ring.front, kSlot1);
        EXPECT_TRUE(d().swaps.empty());
        EXPECT_FALSE(d().transferred);
        EXPECT_FALSE(d().gpuScanout);
        EXPECT_EQ(d().unshownRetired, 0u);
        EXPECT_EQ(logCount("not shown"), 0u);
    }
}

TEST_F(DisplayPromiseTest, PromisedDirectSwapNeverKeptHitsBoundLogsErrorAndKeepsPreviousFrame)
{
    constexpr int kSlot0 = Display::kSlot0, kSlot1 = Display::kSlot0 + 1, kSlot2 = Display::kSlot0 + 2;
    presentInitialFrame(kSlot0);
    gLog.clear();

    backings[1].promises = 1;
    gOffer = [&](const TegraScanoutCopy *) {
        EXPECT_EQ(kernel.parkPromisedOffer(), 0);
        return 0;
    };

    const auto *req1 = d().enqueueSwap();
    const uint32_t swap1 = req1->swap;
    ASSERT_TRUE(d().offerDirectSwap(kSlot1, req1));
    EXPECT_EQ(d().completionFlip.state(), SwitchCompletionFlip::kArmed);

    gNowNs += TegraGM20BPromiseBoundNs - 100000000ULL;
    kernel.runGiveUp();
    d().boundaryAndTick();
    d().checkFlipDeadline();
    EXPECT_EQ(d().completionFlip.state(), SwitchCompletionFlip::kArmed);
    EXPECT_EQ(kernel.steppedRaised, 0u);
    EXPECT_EQ(d().activeEntry(), kSlot0);
    EXPECT_EQ(d().swaps.size(), 1u);

    d().model.written.clear();
    gNowNs += 100000000ULL;
    kernel.runGiveUp();
    EXPECT_EQ(kernel.steppedRaising, 1u);
    EXPECT_EQ(kernel.steppedRaised, 1u);
    EXPECT_EQ(kernel.lastClaim, SWITCH_GPU_PRESENT_WATCH_FAILED);
    EXPECT_TRUE(d().model.written.empty()) << "abandoned promise must withdraw completion flip with zero DC writes";
    EXPECT_EQ(d().completionFlip.state(), SwitchCompletionFlip::kIdle);

    d().donePass();
    d().boundaryAndTick();

    EXPECT_EQ(d().activeEntry(), kSlot0) << "panel must keep previous frame, never show unwritten surface";
    EXPECT_EQ(d().ring.front, kSlot0);
    EXPECT_EQ(std::count(d().shownEntries.begin(), d().shownEntries.end(), kSlot1), 0);
    EXPECT_TRUE(d().swaps.empty()) << "unkept promise swap must retire unshown rather than hanging";
    EXPECT_EQ(d().unshownRetired, 1u);
    EXPECT_EQ(d().unshownSwaps, (std::vector<uint32_t>{swap1}));
    EXPECT_FALSE(d().transferred);
    EXPECT_FALSE(d().gpuScanout);
    char expectedLine[160];
    snprintf(expectedLine, sizeof(expectedLine),
             "SwitchDisplay: swap %u not shown (direct path): its frame's write was never accepted (%d)\n", swap1,
             TEGRA_SCANOUT_SOURCE_REFUSED);
    EXPECT_EQ(logCount(expectedLine), 1u);

    backings[2].promises = 0;
    const auto *req2 = d().enqueueSwap();
    ASSERT_TRUE(d().offerDirectSwap(kSlot2, req2));
    d().boundaryAndTick();
    EXPECT_EQ(d().activeEntry(), kSlot2);
    EXPECT_EQ(d().ring.front, kSlot2);
    EXPECT_TRUE(d().swaps.empty());
}

TEST_F(DisplayPromiseTest, WithdrawnRefusedOrCopyPromiseLogsErrorAndLostStillPanics)
{
    constexpr int kSlot0 = Display::kSlot0, kSlot1 = Display::kSlot0 + 1;
    presentInitialFrame(kSlot0);
    gLog.clear();

    backings[1].promises = 1;
    gOffer = [&](const TegraScanoutCopy *) {
        EXPECT_EQ(kernel.parkPromisedOffer(), 0);
        return 0;
    };
    const auto *reqWithdraw = d().enqueueSwap();
    const uint32_t swapWithdraw = reqWithdraw->swap;
    ASSERT_TRUE(d().offerDirectSwap(kSlot1, reqWithdraw));
    d().model.written.clear();
    kernel.withdrawBeforeBound();
    EXPECT_TRUE(d().model.written.empty());
    d().donePass();
    d().boundaryAndTick();
    EXPECT_EQ(d().activeEntry(), kSlot0);
    EXPECT_TRUE(d().swaps.empty());
    char line[160];
    snprintf(line, sizeof(line),
             "SwitchDisplay: swap %u not shown (direct path): its frame's write was never accepted (%d)\n",
             swapWithdraw, TEGRA_SCANOUT_SOURCE_REFUSED);
    EXPECT_EQ(logCount(line), 1u);

    gOffer = [](const TegraScanoutCopy *) {
        return TEGRA_SCANOUT_SOURCE_REFUSED;
    };
    d().model.written.clear();
    const auto *reqOfferRefused = d().enqueueSwap();
    const uint32_t swapOfferRefused = reqOfferRefused->swap;
    ASSERT_TRUE(d().offerDirectSwap(kSlot1, reqOfferRefused));
    EXPECT_TRUE(d().model.written.empty());
    EXPECT_EQ(d().completionFlip.state(), SwitchCompletionFlip::kIdle);
    EXPECT_TRUE(d().swaps.empty());
    snprintf(line, sizeof(line),
             "SwitchDisplay: swap %u not shown (direct path): its frame's write was never accepted (%d)\n",
             swapOfferRefused, TEGRA_SCANOUT_SOURCE_REFUSED);
    EXPECT_EQ(logCount(line), 1u);

    kernel.reset(&d());
    gOffer = [&](const TegraScanoutCopy *) {
        EXPECT_EQ(kernel.parkPromisedOffer(), 0);
        return 0;
    };
    const auto *reqCopy = d().enqueueSwap();
    const uint32_t swapCopy = reqCopy->swap;
    ASSERT_TRUE(d().offerCopySwap(reqCopy));
    EXPECT_NE(d().ring.drawing, Display::kNoBuffer);
    gNowNs += TegraGM20BPromiseBoundNs;
    kernel.runGiveUp();
    EXPECT_EQ(kernel.steppedRaised, 1u);
    EXPECT_EQ(kernel.lastClaim, SWITCH_GPU_PRESENT_WATCH_FAILED);
    d().donePass();
    EXPECT_EQ(d().ring.drawing, Display::kNoBuffer);
    EXPECT_TRUE(d().swaps.empty());
    snprintf(line, sizeof(line),
             "SwitchDisplay: swap %u not shown (GPU copy path): its frame's write was never accepted (%d)\n", swapCopy,
             TEGRA_SCANOUT_SOURCE_REFUSED);
    EXPECT_EQ(logCount(line), 1u);

    gOffer = [](const TegraScanoutCopy *) {
        return 0;
    };
    const auto *reqLost = d().enqueueSwap();
    ASSERT_TRUE(d().offerDirectSwap(kSlot1, reqLost));
    d().gpuComplete(TEGRA_SCANOUT_SOURCE_LOST);
    EXPECT_THROW(d().donePass(), std::runtime_error);
}

TEST(DisplayPromiseBoundSource, DisplayDeadlineExceedsKernelPromiseBound)
{
    const std::string display = SourceText("../NativeLegacyDisplay.cpp");
    EXPECT_NE(display.find("static constexpr unsigned kGpuCopyDeadlineMS = 10000;"), std::string::npos);
    static_assert(Display::kGpuCopyDeadlineMS * 1000000ULL > TegraGM20BPromiseBoundNs,
                  "display GPU deadline must exceed the kernel writer-promise bound");
}

}  // namespace
}  // namespace display_promise_test
