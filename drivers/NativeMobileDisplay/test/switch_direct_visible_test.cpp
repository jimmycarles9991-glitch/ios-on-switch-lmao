// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchDirectVisibleDisplay.h: a direct scanout slot holds its backing as scanned for its whole life, and a swap whose
// producer's writes are already in DRAM flips from the kick with no present job; one still running past the slot's first
// frame is watched by the GPU driver instead of a present job; anything else keeps the job.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../../TegraDisplayFlip/TegraScanoutCopy.h"
#include "../../TegraVIC/SwitchNativeScanout.h"
#include "bazel/testing/source_text.h"

using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::SourceText;

using IOReturn = int;
constexpr IOReturn kIOReturnSuccess = 0;
#define IOLog(...) ((void)0)

struct switch_gpu_backing {
    unsigned refs = 1, holds = 0;
    int promises = 0;
};

namespace {
int visible = 1;
}  // namespace

extern "C" void switch_gpu_backing_retain(switch_gpu_backing *b)
{
    ++b->refs;
}

extern "C" void switch_gpu_backing_release(switch_gpu_backing *b)
{
    --b->refs;
}

extern "C" void switch_gpu_backing_scanout(switch_gpu_backing *b, int add)
{
    add ? ++b->holds : --b->holds;
}

extern "C" int switch_gpu_backing_token_external_status(const switch_gpu_backing_token *)
{
    return visible;
}

extern "C" int switch_gpu_backing_promised(switch_gpu_backing *b)
{
    return b ? b->promises : 0;
}

namespace {
struct OriginalLegacySwapRequest {};

struct PlaneSet {
    bool fenced = false;
};

struct NativeLegacyDisplay {
    static constexpr int kSlot0 = 4;
    static constexpr unsigned kSlots = 4;
    static constexpr bool stats = true;

    struct DirectSlot {
        SwitchNativeScanout *read = nullptr;
        uint64_t used = 0;
    };

    DirectSlot slots[kSlots];
    SwitchNativeScanout reads[kSlots];
    bool planeNextSet = false;
    PlaneSet planeNext;
    uint64_t alsoWritten = 0, gpuLength = 0;
    uint32_t stDirect = 0;
    const OriginalLegacySwapRequest *flipRequest = nullptr;
    std::vector<int> presented;
    unsigned taken = 0, observed = 0;

    NativeLegacyDisplay()
    {
        for (unsigned k = 0; k < kSlots; ++k)
            slots[k].read = &reads[k];
    }

    uint64_t planeNextAlsoWritten() const
    {
        return alsoWritten;
    }

    bool planeFenced(const PlaneSet &p) const
    {
        return p.fenced;
    }

    void noteTaken(const OriginalLegacySwapRequest *, uint64_t)
    {
        ++taken;
    }

    void presentEntry(int entry)
    {
        presented.push_back(entry);
    }

    void native_legacy_transfer_observed(IOReturn, const OriginalLegacySwapRequest *, uint64_t)
    {
        ++observed;
    }

#include "../SwitchDirectVisibleDisplay.h"
};

class DirectVisible : public testing::Test {
protected:
    NativeLegacyDisplay display;
    OriginalLegacySwapRequest request;
    switch_gpu_backing backing;

    void SetUp() override
    {
        visible = 1;
        display.reads[1].lease = reinterpret_cast<switch_gpu_cpu_lease *>(1);
        display.reads[1].view.native = &backing;
        display.reads[1].view.producer = {7, 1, SWITCH_GPU_ENGINE_GM20B, 3};
    }
};

TEST_F(DirectVisible, ASlotHoldsItsBackingScannedUntilDropped)
{
    display.holdScanoutBacking(1);
    EXPECT_EQ(backing.holds, 1u);
    EXPECT_EQ(backing.refs, 2u);
    display.reads[1] = {};  // the read lease goes between frames; the hold stays with the slot
    EXPECT_EQ(backing.holds, 1u);
    display.holdScanoutBacking(1);
    EXPECT_EQ(backing.holds, 0u);
    display.releaseScanoutBacking(1);
    EXPECT_EQ(backing.refs, 1u);
}

TEST_F(DirectVisible, AVisibleProducerFlipsFromTheKick)
{
    EXPECT_TRUE(display.presentDirectVisible(&request, NativeLegacyDisplay::kSlot0 + 1, 1280, 720, 5));
    ASSERT_EQ(display.presented.size(), 1u);
    EXPECT_EQ(display.presented[0], NativeLegacyDisplay::kSlot0 + 1);
    EXPECT_EQ(display.flipRequest, &request);
    EXPECT_EQ(display.taken, 1u);
    EXPECT_EQ(display.observed, 1u);
    EXPECT_EQ(display.gpuLength, 1280u * 720u * 4u);
    EXPECT_EQ(display.stDirect, 1u);
}

TEST_F(DirectVisible, AProducerNotYetInDramKeepsThePresentJob)
{
    visible = 0;
    EXPECT_FALSE(display.presentDirectVisible(&request, NativeLegacyDisplay::kSlot0 + 1, 1280, 720, 5));
    visible = -5;
    EXPECT_FALSE(display.presentDirectVisible(&request, NativeLegacyDisplay::kSlot0 + 1, 1280, 720, 5));
    EXPECT_TRUE(display.presented.empty());
    EXPECT_EQ(display.taken, 0u);
}

// The DC's flip has no GPU work in front of it to acquire a writer promise in: the fence-only present job does.
TEST_F(DirectVisible, APromisedSurfaceKeepsThePresentJobWhateverItsProducer)
{
    backing.promises = 1;
    EXPECT_FALSE(display.presentDirectVisible(&request, NativeLegacyDisplay::kSlot0 + 1, 1280, 720, 5));
    display.reads[1].view.producer = {};
    EXPECT_FALSE(display.presentDirectVisible(&request, NativeLegacyDisplay::kSlot0 + 1, 1280, 720, 5));
    EXPECT_TRUE(display.presented.empty());
    backing.promises = 0;
    EXPECT_TRUE(display.presentDirectVisible(&request, NativeLegacyDisplay::kSlot0 + 1, 1280, 720, 5));
}

TEST_F(DirectVisible, NoEngineWriterFlipsAndNoLeaseDoesNot)
{
    display.reads[1].view.producer = {};
    visible = 0;
    EXPECT_TRUE(display.presentDirectVisible(&request, NativeLegacyDisplay::kSlot0 + 1, 1280, 720, 5));
    display.reads[1].lease = nullptr;
    EXPECT_FALSE(display.presentDirectVisible(&request, NativeLegacyDisplay::kSlot0 + 1, 1280, 720, 5));
}

TEST_F(DirectVisible, AWindowBChangeWithWritersOfItsOwnKeepsThePresentJob)
{
    display.alsoWritten = 0x1000;
    EXPECT_FALSE(display.presentDirectVisible(&request, NativeLegacyDisplay::kSlot0 + 1, 1280, 720, 5));
    display.alsoWritten = 0;
    display.planeNextSet = true;
    display.planeNext.fenced = true;
    EXPECT_FALSE(display.presentDirectVisible(&request, NativeLegacyDisplay::kSlot0 + 1, 1280, 720, 5));
    display.planeNext.fenced = false;
    EXPECT_TRUE(display.presentDirectVisible(&request, NativeLegacyDisplay::kSlot0 + 1, 1280, 720, 5));
}

TEST_F(DirectVisible, ASlotsFirstFrameKeepsThePresentJobAndLaterOnesAreWatched)
{
    const int entry = NativeLegacyDisplay::kSlot0 + 1;
    EXPECT_EQ(display.producerWatch(entry), 0u);  // nothing held: its jobs were never made External
    display.slots[1].used = 9;
    display.holdScanoutBacking(1);
    EXPECT_EQ(display.producerWatch(entry), 0u);  // written before the hold
    display.slots[1].used = 12;
    EXPECT_EQ(display.producerWatch(entry), TEGRA_SCANOUT_COPY_PRODUCER_WATCH);
    display.releaseScanoutBacking(1);
    EXPECT_EQ(display.producerWatch(entry), 0u);
}

TEST(DirectVisibleSource, TheOfferTriesItOnceTheSlotIsMappedAndSlotsHoldTheirBacking)
{
    const std::string display = SourceText("../NativeLegacyDisplay.cpp");
    const CodeText offer = Block(display, "bool offerDirect(");
    EXPECT_TRUE(offer.Before("if (entry == kNoBuffer)", "presentDirectVisible(request, entry, width"));
    EXPECT_TRUE(offer.Before("presentDirectVisible(request, entry, width", "offerWithCompletionFlip(copy, entry)"));
    EXPECT_TRUE(offer.Has("copy.flags = TEGRA_SCANOUT_COPY_FENCE_ONLY | producerWatch(entry);"));
    const std::string slots = SourceText("../SwitchDirectSlots.h");
    EXPECT_TRUE(Block(slots, "void dropSlot(unsigned k)").Has("releaseScanoutBacking(k);"));
    EXPECT_TRUE(CodeText(slots).Has("*slot.read = snapshot; holdScanoutBacking(static_cast<unsigned>(victim));"));
}
}  // namespace
