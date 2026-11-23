// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchDirectVisibleDisplay.h: a direct scanout slot holds its backing as scanned for its whole life, and a swap whose
// producer's writes are already in DRAM flips from the kick with no present job; one still running past the slot's first
// frame is watched by the GPU driver instead of a present job; anything else keeps the job.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
namespace switch_gpu_refusal {
// SwitchGpuRefusalSites.h's bound, which this test's sources do not carry
constexpr bool logged(uint32_t nth)
{
    return nth < 4 || nth % 256 == 0;
}
}  // namespace switch_gpu_refusal

namespace {
std::vector<std::string> lines;

void IOLog(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    lines.emplace_back(line);
}
}  // namespace
#endif

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
struct OriginalLegacySwapRequest {
    uint32_t swap = 0;
};

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

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
    struct {
        int front = 0;
    } ring;

    uint64_t seqOf(const OriginalLegacySwapRequest *) const
    {
        return taken;
    }

    static uint32_t swapOf(const OriginalLegacySwapRequest *request)
    {
        return request->swap;
    }
#endif

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
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
        lines.clear();
#endif
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

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
size_t linesWith(const std::string &text)
{
    return static_cast<size_t>(std::count_if(lines.begin(), lines.end(), [&](const std::string &line) {
        return line.find(text) != std::string::npos;
    }));
}

// What directSlotFor copies from the swap's snapshot: surface 33, allocation 9, at content generation `content`.
switch_gpu_scanout_look drawn(uint64_t content)
{
    switch_gpu_scanout_look look = {};
    look.allocation = 9;
    look.content = content;
    look.surface = 33;
    return look;
}

TEST_F(DirectVisible, AFlipToContentsAlreadyShownNamesTheFrameThatShowedThem)
{
    display.directLook = drawn(4);
    request.swap = 41;
    EXPECT_TRUE(display.presentDirectVisible(&request, NativeLegacyDisplay::kSlot0 + 1, 1280, 720, 5));
    request.swap = 42;
    EXPECT_TRUE(display.presentDirectVisible(&request, NativeLegacyDisplay::kSlot0 + 1, 1280, 720, 5));
    EXPECT_EQ(linesWith("swap 42 (taken 2) flips from slot 0 to surface 33 (allocation 9) in slot 5 with no present "
                        "job, again with nothing written since swap 41 (taken 1), 1 frames ago (generation 4,"),
              1u);
    EXPECT_EQ(linesWith("nothing else changes with it (1 so far)"), 1u);
}

// A pending writer promise is a write still to come, whatever the generation.
TEST_F(DirectVisible, AFlipToNewOrPromisedContentsLogsNothing)
{
    display.directLook = drawn(4);
    EXPECT_TRUE(display.presentDirectVisible(&request, NativeLegacyDisplay::kSlot0 + 1, 1280, 720, 5));
    display.directLook.content = 5;
    EXPECT_TRUE(display.presentDirectVisible(&request, NativeLegacyDisplay::kSlot0 + 1, 1280, 720, 5));
    display.directLook.promised = 1;
    EXPECT_TRUE(display.presentDirectVisible(&request, NativeLegacyDisplay::kSlot0 + 1, 1280, 720, 5));
    EXPECT_EQ(linesWith("again with nothing written"), 0u);
}
#endif

TEST(DirectVisibleSource, TheOfferTriesItOnceTheSlotIsMappedAndSlotsHoldTheirBacking)
{
    const std::string display = SourceText("../NativeLegacyDisplay.cpp");
    const CodeText offer = Block(display, "bool offerDirect(");
    EXPECT_TRUE(offer.before("if (entry == kNoBuffer)", "presentDirectVisible(request, entry, width"));
    EXPECT_TRUE(offer.before("presentDirectVisible(request, entry, width", "offerWithCompletionFlip(copy, entry)"));
    EXPECT_TRUE(offer.has("copy.flags = TEGRA_SCANOUT_COPY_FENCE_ONLY | producerWatch(entry) | (tiled ? "
                          "TEGRA_SCANOUT_COPY_SOURCE_BLOCK : 0u);"))
        << "a tiled slot's present names its blocks: mapped as pitch, ui.glass_live's read-back copied scrambled rows";
    EXPECT_TRUE(offer.has("copy.sourceBlockHeightLog2 = tiled ? kScanBlockLog2 : 0u;"));
    const std::string slots = SourceText("../SwitchDirectSlots.h");
    EXPECT_TRUE(Block(slots, "void dropSlot(unsigned k)").has("releaseScanoutBacking(k);"));
    EXPECT_TRUE(CodeText(slots).has("*slot.read = snapshot; holdScanoutBacking(static_cast<unsigned>(victim));"));
}
}  // namespace
