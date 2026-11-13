// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include "../SwitchDisplayComposition.h"
#include "../SwitchLandscapeWindows.h"

namespace {
using namespace native_planes;

struct Display {
    using PlaneSet = switch_display::Composition;
    using Commits = switch_display::Compositions<PlaneSet>;
    static constexpr unsigned kWinBBase = 0xd80, kWinbufBBase = 0xdc0, kDcStateAccess = 0x40;
    static constexpr uint32_t kReadMuxActive = 1, kWriteMuxActive = 4, kWinEnable = 1u << 30;
    std::array<uint32_t, 4096> registers = {};
    uint32_t *dc = registers.data();

    struct Slot {
        uint64_t low = 0;
    } planeSlots[8];

    PlaneSet sets[Commits::kSlots + 1];
    Commits planeLatch;

    Display()
    {
        planeLatch.values = sets;
    }

    static uint32_t planeWindowBase(unsigned window)
    {
        return window ? 0xf80 : kWinBBase;
    }

    static uint32_t planeBufferBase(unsigned window)
    {
        return window ? 0xfc0 : kWinbufBBase;
    }

    uint32_t planeSlotIova(int slot)
    {
        return 0x10000000 + uint32_t(slot) * 0x100000;
    }

    PlaneSet &planeSetOf(int id)
    {
        return sets[id >= 0 ? id : Commits::kSlots];
    }

    using Window = switch_landscape::Window;

    static Window planeWindowOf(const PlaneSet &set, unsigned index)
    {
        return switch_landscape::windowsOf(set.landscape).overlay[index];
    }

#include "../SwitchCompositionRegisters.h"

    void load(const PlaneSet &p)
    {
        for (unsigned window = 0; window != 2; ++window) {
            const auto &overlay = p.window[window];
            auto *win = dc + planeWindowBase(window), *buf = dc + planeBufferBase(window);
            win[kOptions] = overlay.on ? overlay.image.options : 0;
            if (!overlay.on)
                continue;
            const auto &m = overlay.image;
            win[kColorDepth] = m.colorDepth;
            win[kPosition] = m.position;
            win[kSize] = m.size;
            win[kPrescaled] = m.prescaled;
            win[kDdaIncrement] = m.dda;
            win[kHInitialDda] = m.hInitial;
            win[kVInitialDda] = m.vInitial;
            win[kLineStride] = m.lineStride;
            win[kBlendLayer] = (m.yuv ? kBlendBypass : kBlendPremultiplied) | overlay.depth;
            win[kBlendMatch] = win[kBlendNomatch] = m.yuv ? 0 : kBlendOver;
            const auto base = planeSlotIova(overlay.slot) - planeSlots[overlay.slot].low;
            buf[kStartAddr] = uint32_t(base + m.start[0]);
            buf[kStartAddrHi] = buf[kStartAddrHiU] = 0;
            buf[kStartAddrU] = m.yuv ? uint32_t(base + m.start[1]) : 0;
            buf[kSurfaceKind] = m.surfaceKind;
            buf[kHOffset] = m.hOffset;
            buf[kVOffset] = m.vOffset;
        }
    }
};

Display::PlaneSet frame(unsigned position)
{
    Display::PlaneSet result;
    result.window[0].on = true;
    result.window[0].slot = 2;
    result.window[0].image.options = Display::kWinEnable;
    result.window[0].image.position = position;
    result.window[0].image.size = 0x1000100;
    result.window[0].image.lineStride = 1024;
    return result;
}

TEST(CompositionRegisters, ReusedSurfaceDistinguishesOldGeometryAndPreservesMux)
{
    Display d;
    const int old = d.planeLatch.write(frame(10));
    d.load(d.sets[old]);
    ASSERT_TRUE(d.planeLatch.latched(false, d.planeScannedSet()));
    const int next = d.planeLatch.write(frame(20));
    d.dc[Display::kDcStateAccess] = 0x80;
    EXPECT_EQ(d.planeScannedSet(), old);
    EXPECT_EQ(d.dc[Display::kDcStateAccess], 0x80u);
    EXPECT_FALSE(d.planeLatch.latched(false, d.planeScannedSet()));
    EXPECT_TRUE(d.planeLatch.uses(2));
    d.load(d.sets[next]);
    EXPECT_EQ(d.planeScannedSet(), next);
    EXPECT_TRUE(d.planeLatch.latched(false, d.planeScannedSet()));
}

TEST(CompositionRegisters, FractionalSamplingAndPlaneAddressArePartOfIdentity)
{
    Display d;
    auto p = frame(0);
    p.window[0].image.yuv = true;
    p.window[0].image.start[1] = 0x2000;
    p.window[0].image.hInitial = 2048;
    const int next = d.planeLatch.write(p);
    d.load(p);
    EXPECT_EQ(d.planeScannedSet(), next);
    d.dc[Display::kWinBBase + kHInitialDda] = 0;
    EXPECT_EQ(d.planeScannedSet(), PlaneLatch::kUnknown);
    d.load(p);
    d.dc[Display::kWinbufBBase + kStartAddrHiU] = 1;
    EXPECT_EQ(d.planeScannedSet(), PlaneLatch::kUnknown);
    d.load(p);
    d.dc[Display::kWinbufBBase + kStartAddrU] += 64;
    EXPECT_EQ(d.planeScannedSet(), PlaneLatch::kUnknown);
}

TEST(CompositionRegisters, IdenticalActiveAndArmedGeometryWaitsForRequestClear)
{
    Display d;
    auto p = frame(0);
    const int old = d.planeLatch.write(p);
    d.load(p);
    ASSERT_TRUE(d.planeLatch.latched(false, d.planeScannedSet()));
    p.seq = 2;
    const int next = d.planeLatch.write(p);
    EXPECT_NE(next, old);
    EXPECT_EQ(d.planeScannedSet(), next);
    EXPECT_FALSE(d.planeLatch.latched(true, d.planeScannedSet()));
    EXPECT_EQ(d.planeLatch.active, old);
    EXPECT_TRUE(d.planeLatch.latched(false, d.planeScannedSet()));
}

TEST(CompositionRegisters, OffCommitStillNeedsDisabledHardware)
{
    Display d;
    auto p = frame(0);
    d.planeLatch.write(p);
    d.load(p);
    ASSERT_TRUE(d.planeLatch.latched(false, d.planeScannedSet()));
    const int off = d.planeLatch.write({});
    EXPECT_NE(d.planeScannedSet(), off);
    d.dc[Display::kWinBBase + kOptions] = 0;
    EXPECT_EQ(d.planeScannedSet(), off);
    EXPECT_TRUE(d.planeLatch.latched(false, d.planeScannedSet()));
    EXPECT_FALSE(d.planeLatch.uses(2));
}
}
