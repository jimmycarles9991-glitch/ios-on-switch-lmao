// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <stdexcept>
#include <vector>
#include "../../TegraSMMU/TegraSMMU.h"
#include "../SwitchDisplayMemoryTiming.h"

struct {
    bool armed = false;
} gPanicRestore;

int restoreResult = 0;
unsigned restores = 0;

extern "C" int switch_smmu_restore(uint32_t asid, uint32_t ptb, uint32_t group, char *, size_t)
{
    EXPECT_EQ(asid, TEGRA_SMMU_ASID_DISPLAY);
    EXPECT_EQ(ptb, 0x12345u);
    EXPECT_EQ(group, TEGRA_SMMU_DC_ASID);
    ++restores;
    return restoreResult;
}

namespace {
unsigned logs = 0;

void IOLog(const char *, ...)
{
    ++logs;
}

[[noreturn]] void panic(const char *, ...)
{
    throw std::runtime_error("restore refused");
}

struct Display {
    enum Reg {
        kDcStateAccess,
        kDcWindowHeader,
        kWinOptions,
        kWinStart,
        kWinKind,
        kWinStride,
        kWinHOffset,
        kDcDispWinOptions,
        kDcRegActControl,
        kDcScratchB,
        kDcStateControl,
        kDcIntStatus,
        kDcIntEnable,
        kCount
    };

    static constexpr uint32_t kWinbufStartAddr = kWinStart, kWinbufSurfaceKind = kWinKind, kWinLineStride = kWinStride,
                              kWinbufHOffset = kWinHOffset;
    static constexpr uint32_t kWindowAState[] = {kWinStart, kWinKind, kWinStride, kWinHOffset, kWinOptions};
    static constexpr uint32_t kBlockSurfaceKind = 0x42, kBlockLineStride = 0x3000, kBlockHOffset = 0xb3f;
    static constexpr uint32_t kWindowASelect = 0x10, kGeneralUpdate = 1 << 8, kWinAUpdate = 1 << 9;
    static constexpr uint32_t kGeneralActReq = 1, kWinAActReq = 2, kVblankInt = 2, kFrameEndInt = 4;
    static constexpr uint32_t kWinAUnderflowInt = 256;
    static constexpr int kNoBuffer = -1;

    struct Loop {
        bool gated = true;

        bool inGate() const
        {
            return gated;
        }
    } loop;

    struct Domain {
        uint32_t ptb() const
        {
            return 0x12345;
        }
    } probeDomain;

    struct Color {
        void restoreAfterPowerUp()
        {}
    } colorObject;

    Color *color = &colorObject;

    struct Source {
        bool enabled = false;

        void enable()
        {
            enabled = true;
        }
    } source;

    Source *vblankSource = &source;

    struct Ring {
        int held = 7;
        unsigned buffers = 3, resets = 0;

        void reset(unsigned)
        {
            ++resets;
            held = -1;
        }

        int flipHeld()
        {
            const auto result = held;
            held = -1;
            return result;
        }
    } ring;

    std::array<uint32_t, kCount> dc{};
    uint32_t savedWindow[5] = {0x10000000, 0x7, 0x8, 0x9, 0x40000000};
    uint32_t savedDispWinOptions = 1, savedActControl = 2, savedScratchB = 3, savedIntEnable = 7, savedIntMask = 6;
    uint32_t dcPending = 9, underflowRun = 3;
    uint64_t frameEnds = 100, rotationRequested = 0;
    bool hwSuspended = true, directAttached = true, directOn = true, planesAttached = true, vpulseProgrammed = true;
    unsigned planesStopped = 0;
    bool scanoutStopped = true, rotated = true, rotationLatched = true, resumeLit = true;
    bool presentDeferred = true, gpuPresentDeferred = true, kickDeferred = true;
    bool timingValid = true, planesRestored = false;
    unsigned started = 0, callbacks = 0, underflowClears = 0;
    std::vector<int> order;

    Display()
    {
        restoreResult = 0;
        restores = 0;
        gPanicRestore = {};
    }

    Loop *getWorkLoop()
    {
        return &loop;
    }

    static uint32_t surfaceAddress(int)
    {
        return 0xf5a00000;
    }

    void stopPlanes()
    {
        ++planesStopped;
    }

    bool captureDisplayMemory(switch_display::TimingBank bank)
    {
        EXPECT_EQ(bank, switch_display::TimingBank::Programmed);
        return timingValid;
    }

    void writePhaseRegisters()
    {
        order.push_back(1);
    }

    void writeIntMask(uint32_t, uint32_t)
    {}

    void clearUnderflowCounts()
    {
        EXPECT_TRUE(hwSuspended);
        ++underflowClears;
    }

    void planesResume()
    {
        EXPECT_TRUE(hwSuspended);
        EXPECT_TRUE(scanoutStopped);
        EXPECT_FALSE(source.enabled);
        EXPECT_EQ(dc[kWinStart], savedWindow[0]);
        order.push_back(2);
        planesRestored = true;
    }

    void setScanout(bool on)
    {
        EXPECT_TRUE(planesRestored);
        EXPECT_FALSE(hwSuspended);
        scanoutStopped = !on;
        ++started;
        order.push_back(3);
    }

    void callback()
    {
        EXPECT_TRUE(planesRestored);
        EXPECT_FALSE(hwSuspended);
        ++callbacks;
        order.push_back(4);
    }

    void presentCopied()
    {
        callback();
    }

    void presentGpuCopy()
    {
        callback();
    }

    void writeFlip(int slot)
    {
        EXPECT_EQ(slot, 7);
        callback();
    }

    void kickSwapQueue()
    {
        callback();
    }

#include "../SwitchDisplayResume.h"
};

TEST(DisplayResume, RestoresCompleteImageBeforeAnyDeferredPresentationOrStart)
{
    Display d;
    ASSERT_TRUE(d.finishResume());
    EXPECT_EQ(restores, 1u);
    EXPECT_EQ(d.dc[Display::kWinStart], 0x10000000u);
    EXPECT_EQ(d.started, 1u);
    EXPECT_EQ(d.callbacks, 4u);
    EXPECT_EQ(d.order, (std::vector<int>{1, 2, 3, 4, 4, 4, 4}));
    EXPECT_TRUE(d.directAttached && d.planesAttached && gPanicRestore.armed);
    EXPECT_EQ(d.underflowClears, 1u);
}

TEST(DisplayResume, DomainThatDoesNotComeBackPutsWindowABackOnItsScanBuffers)
{
    Display d;
    restoreResult = -1;
    EXPECT_TRUE(d.finishResume());
    EXPECT_FALSE(d.directAttached || d.directOn || d.planesAttached);
    EXPECT_EQ(d.planesStopped, 1u);
    EXPECT_EQ(d.ring.resets, 1u);
    EXPECT_EQ(d.dc[Display::kWinStart], 0xf5a00000u);
    EXPECT_EQ(d.dc[Display::kWinKind], Display::kBlockSurfaceKind);
    EXPECT_EQ(d.dc[Display::kWinStride], Display::kBlockLineStride);
    EXPECT_EQ(d.dc[Display::kWinHOffset], Display::kBlockHOffset);
    EXPECT_EQ(d.started, 1u);
    EXPECT_TRUE(d.source.enabled);
}

TEST(DisplayResume, SleepingPanelStillRestoresItsAcceptedState)
{
    Display d;
    d.resumeLit = false;
    EXPECT_FALSE(d.finishResume());
    EXPECT_TRUE(d.planesRestored && d.scanoutStopped);
    EXPECT_EQ(d.started, 0u);
    EXPECT_EQ(d.callbacks, 4u);
}

TEST(DisplayResume, UngatedRestoreCannotPublishRestoredRegisters)
{
    Display d;
    d.loop.gated = false;
    EXPECT_THROW(d.finishResume(), std::runtime_error);
    EXPECT_EQ(restores, 0u);
    EXPECT_EQ(d.callbacks + d.started, 0u);
}

TEST(DisplayResume, InvalidTimingTurnsAdmissionOffAndStillLightsThePanel)
{
    Display d;
    d.timingValid = false;
    logs = 0;
    EXPECT_TRUE(d.finishResume());
    EXPECT_EQ(logs, 1u);
    EXPECT_EQ(d.started, 1u);
    EXPECT_FALSE(d.hwSuspended);
}
}
