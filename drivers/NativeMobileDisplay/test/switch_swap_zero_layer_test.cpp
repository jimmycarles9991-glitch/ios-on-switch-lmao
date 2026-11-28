// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <stdint.h>
#include <string.h>

#include "../SwitchSwapZeroLayer.h"

namespace {

using switch_display::SwapZeroLayerState;

struct FakeSwap {
    unsigned char bytes[0x200] = {};

    void setEnabled(uint32_t mask)
    {
        memcpy(bytes + SwapZeroLayerState::kSwapEnabledOffset, &mask, sizeof(mask));
    }

    uint32_t enabled() const
    {
        uint32_t mask = 0;
        memcpy(&mask, bytes + SwapZeroLayerState::kSwapEnabledOffset, sizeof(mask));
        return mask;
    }

    void setSurfaceId(unsigned layer, uint32_t sid)
    {
        memcpy(bytes + SwapZeroLayerState::kSwapSurfaceIdsOffset + 4 * layer, &sid, sizeof(sid));
    }

    void setType(uint32_t type)
    {
        memcpy(bytes + SwapZeroLayerState::kSwapTypeOffset, &type, sizeof(type));
    }
};

}  // namespace

TEST(SwitchSwapZeroLayer, ElidesRedundantZeroSurfaceLayerAfterFirstDisable)
{
    SwapZeroLayerState state;
    FakeSwap swap;
    swap.setEnabled(0x7u);  // layers 0 (RGB detached), 1 (UI), 2 (NULL)
    swap.setSurfaceId(0, 101);
    swap.setSurfaceId(1, 102);
    swap.setSurfaceId(2, 0);

    // First disable of layer 2 must pass through so the family clears layer 2.
    auto c1 = state.begin(swap.bytes);
    EXPECT_EQ(c1.elided, 0u);
    EXPECT_EQ(swap.enabled(), 0x7u);
    state.finish(swap.bytes, c1, true);
    EXPECT_EQ(swap.enabled(), 0x7u);

    // Steady-state detached frame: layer 2 is already off, so bit 2 is elided during swap_submit and restored after.
    auto c2 = state.begin(swap.bytes);
    EXPECT_EQ(c2.elided, 1u << 2);
    EXPECT_EQ(swap.enabled(), 0x3u);
    state.finish(swap.bytes, c2, true);
    EXPECT_EQ(swap.enabled(), 0x7u);
}

TEST(SwitchSwapZeroLayer, ReEnablingLayerForcesNextDisableThrough)
{
    SwapZeroLayerState state;
    FakeSwap swap;
    swap.setEnabled(0x5u);  // layer 0 active, layer 2 off
    swap.setSurfaceId(0, 11);
    swap.setSurfaceId(2, 0);
    state.finish(swap.bytes, state.begin(swap.bytes), true);

    // Layer 2 becomes active (e.g. detached YUV video), layer 0 disabled.
    swap.setSurfaceId(0, 0);
    swap.setSurfaceId(2, 22);
    auto cOn = state.begin(swap.bytes);
    EXPECT_EQ(cOn.elided, 0u);
    EXPECT_EQ(swap.enabled(), 0x5u);
    state.finish(swap.bytes, cOn, true);

    // Next frame with layer 2 active and layer 0 still 0 elides bit 0.
    auto cYuvSteady = state.begin(swap.bytes);
    EXPECT_EQ(cYuvSteady.elided, 1u << 0);
    EXPECT_EQ(swap.enabled(), 0x4u);
    state.finish(swap.bytes, cYuvSteady, true);

    // Switching back to layer 0 active and layer 2 off passes bit 2 through once, then elides it again.
    swap.setSurfaceId(0, 33);
    swap.setSurfaceId(2, 0);
    auto cOff = state.begin(swap.bytes);
    EXPECT_EQ(cOff.elided, 0u);
    EXPECT_EQ(swap.enabled(), 0x5u);
    state.finish(swap.bytes, cOff, true);

    auto cRgbSteady = state.begin(swap.bytes);
    EXPECT_EQ(cRgbSteady.elided, 1u << 2);
    EXPECT_EQ(swap.enabled(), 0x1u);
    state.finish(swap.bytes, cRgbSteady, true);
}

TEST(SwitchSwapZeroLayer, FailedSubmitDoesNotClearActiveState)
{
    SwapZeroLayerState state;
    FakeSwap swap;
    swap.setEnabled(0x5u);
    swap.setSurfaceId(0, 10);
    swap.setSurfaceId(2, 0);

    auto c1 = state.begin(swap.bytes);
    EXPECT_EQ(c1.elided, 0u);
    state.finish(swap.bytes, c1, false);

    auto c2 = state.begin(swap.bytes);
    EXPECT_EQ(c2.elided, 0u);
    state.finish(swap.bytes, c2, true);

    auto c3 = state.begin(swap.bytes);
    EXPECT_EQ(c3.elided, 1u << 2);
    state.finish(swap.bytes, c3, true);
}

TEST(SwitchSwapZeroLayer, ConcurrentActivatePreventsDisableFromMarkingOff)
{
    SwapZeroLayerState state;
    FakeSwap disableSwap;
    disableSwap.setEnabled(0x5u);
    disableSwap.setSurfaceId(0, 10);
    disableSwap.setSurfaceId(2, 0);

    FakeSwap activateSwap;
    activateSwap.setEnabled(0x4u);
    activateSwap.setSurfaceId(2, 99);

    auto cDisable = state.begin(disableSwap.bytes);
    auto cActivate = state.begin(activateSwap.bytes);
    state.finish(disableSwap.bytes, cDisable, true);
    state.finish(activateSwap.bytes, cActivate, true);

    // Layer 2 was activated concurrently, so the next disable must not be elided.
    auto cAfter = state.begin(disableSwap.bytes);
    EXPECT_EQ(cAfter.elided, 0u);
    state.finish(disableSwap.bytes, cAfter, true);
}

TEST(SwitchSwapZeroLayer, NeverEmptiesNonZeroEnabledMaskAndRespectsSpecialAndReset)
{
    SwapZeroLayerState state;
    FakeSwap swap;
    swap.setEnabled(0x5u);
    swap.setSurfaceId(0, 10);
    swap.setSurfaceId(2, 0);
    state.finish(swap.bytes, state.begin(swap.bytes), true);

    // Only layer 2 bit set: do not collapse non-zero enabled mask to 0.
    FakeSwap onlyLayer2;
    onlyLayer2.setEnabled(0x4u);
    onlyLayer2.setSurfaceId(2, 0);
    auto cOnly = state.begin(onlyLayer2.bytes);
    EXPECT_EQ(cOnly.elided, 0u);
    EXPECT_EQ(onlyLayer2.enabled(), 0x4u);
    state.finish(onlyLayer2.bytes, cOnly, true);

    // Background bit + already-off layer 2: elides bit 2, keeps bit 31.
    FakeSwap bgAndOff;
    bgAndOff.setEnabled(0x80000004u);
    bgAndOff.setSurfaceId(2, 0);
    auto cBg = state.begin(bgAndOff.bytes);
    EXPECT_EQ(cBg.elided, 1u << 2);
    EXPECT_EQ(bgAndOff.enabled(), 0x80000000u);
    state.finish(bgAndOff.bytes, cBg, true);
    EXPECT_EQ(bgAndOff.enabled(), 0x80000004u);

    // Special swap type 9 uses an internal surface: never elide and conservatively mark all layers active.
    FakeSwap special;
    special.setEnabled(0x5u);
    special.setSurfaceId(0, 10);
    special.setSurfaceId(2, 0);
    special.setType(SwapZeroLayerState::kSwapTypeInternal);
    auto cSpec = state.begin(special.bytes);
    EXPECT_EQ(cSpec.elided, 0u);
    state.finish(special.bytes, cSpec, true);

    auto cAfterSpec = state.begin(swap.bytes);
    EXPECT_EQ(cAfterSpec.elided, 0u);
    state.finish(swap.bytes, cAfterSpec, true);

    // reset() invalidates both current activeMask and any in-flight disable started before reset.
    auto cInFlight = state.begin(swap.bytes);
    state.reset();
    state.finish(swap.bytes, cInFlight, true);
    auto cAfterReset = state.begin(swap.bytes);
    EXPECT_EQ(cAfterReset.elided, 0u);
    state.finish(swap.bytes, cAfterReset, true);
}

TEST(SwitchSwapZeroLayer, ConcurrentActivateBeforeDisablePreventsClearingActiveState)
{
    SwapZeroLayerState state;
    FakeSwap disableSwap;
    disableSwap.setEnabled(0x5u);
    disableSwap.setSurfaceId(0, 10);
    disableSwap.setSurfaceId(2, 0);

    FakeSwap activateSwap;
    activateSwap.setEnabled(0x4u);
    activateSwap.setSurfaceId(2, 99);

    auto cActivate = state.begin(activateSwap.bytes);
    auto cDisable = state.begin(disableSwap.bytes);
    state.finish(disableSwap.bytes, cDisable, true);
    state.finish(activateSwap.bytes, cActivate, true);

    auto cAfter = state.begin(disableSwap.bytes);
    EXPECT_EQ(cAfter.elided, 0u);
    state.finish(disableSwap.bytes, cAfter, true);

    cActivate = state.begin(activateSwap.bytes);
    cDisable = state.begin(disableSwap.bytes);
    state.finish(activateSwap.bytes, cActivate, true);
    state.finish(disableSwap.bytes, cDisable, true);

    cAfter = state.begin(disableSwap.bytes);
    EXPECT_EQ(cAfter.elided, 0u);
    state.finish(disableSwap.bytes, cAfter, true);
}
