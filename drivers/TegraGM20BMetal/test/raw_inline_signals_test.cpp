// SPDX-License-Identifier: MIT
// raw_inline_signals.h and raw_event_release.h: a kernel event's signal between two native ranges joins them into one job,
// its release the kernel's external fence sequence into the event's word; and the window a payload must fall in. That the
// facade keeps such a command buffer whole is raw_facade_lazy_test's ASignalBetweenTwoPassesSplitsNoJob.
#include <cstdint>
#include <string>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_event_release.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_inline_signals.h"

namespace {
using namespace raw;
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

std::vector<InlineRangeView> ranges(const char *shape)
{
    std::vector<InlineRangeView> out;
    for (const char *c = shape; *c; ++c)
        out.push_back({*c == 'N', *c == 's'});
    return out;
}

TEST(RawInlineSignals, SignalsBetweenNativeRangesJoinThem)
{
    EXPECT_TRUE(inlineJoins(ranges("NsN")).size() == 1);
    const auto chain = inlineJoins(ranges("NsNssNsW"));
    ASSERT_EQ(chain.size(), 1u);
    EXPECT_EQ(chain[0].first, 0u);
    EXPECT_EQ(chain[0].last, 5u);
    EXPECT_TRUE(inlineJoins(ranges("Ns")).empty());
    EXPECT_TRUE(inlineJoins(ranges("sN")).empty());
    EXPECT_TRUE(inlineJoins(ranges("NN")).empty());
    EXPECT_TRUE(inlineJoins(ranges("NsWsN")).empty());
    EXPECT_TRUE(inlineJoins(ranges("NsMN")).empty());
    const auto two = inlineJoins(ranges("NsNMNsN"));
    ASSERT_EQ(two.size(), 2u);
    EXPECT_EQ(two[1].first, 4u);
    EXPECT_EQ(two[1].last, 6u);
}

TEST(RawInlineSignals, AReleaseAfterHostVisibleWritesIsTheKernelsExternalFenceIntoTheWord)
{
    uint32_t words[kEventReleaseWords];
    Push p{words, words + kEventReleaseWords};
    emitEventRelease(p, 0xff'fffe'0840ull, 9, true);
    ASSERT_EQ(p.at, words + kEventReleaseWords);
    EXPECT_EQ(words[0], Push::hdr(1, 0, NVB06F_WFI, 1));
    EXPECT_EQ(words[2], Push::hdr(1, 0, NVB06F_FB_FLUSH, 1));
    EXPECT_EQ(words[4], Push::hdr(1, 0, NVB06F_MEM_OP_C, 2));
    EXPECT_EQ(words[6], 0x10u << 27);  // L2_FLUSH_DIRTY (gpu_internal.h)
    EXPECT_EQ(words[7], Push::hdr(1, 0, NVB06F_FB_FLUSH, 1));
    EXPECT_EQ(words[9], Push::hdr(1, 0, NVB06F_SEMAPHOREA, 4));
    EXPECT_EQ(words[10], 0xffu);
    EXPECT_EQ(words[11], 0xfffe0840u);
    EXPECT_EQ(words[12], 9u);
    // SEMAPHORED: OPERATION RELEASE, RELEASE_WFI EN (0), RELEASE_SIZE 4BYTE (24:24)
    EXPECT_EQ(words[13], NVB06F_SEMAPHORED_OPERATION_RELEASE | NVB06F_SEMAPHORED_RELEASE_SIZE_4BYTE << 24);
    EXPECT_EQ(words[14], Push::hdr(1, 0, NVB06F_NON_STALL_INTERRUPT, 1));
    const std::string fence = Body(SourceText("../../TegraGM20B/gpu_exec.c"), "static u32 chan_fence_job(");
    for (const char *step :
         {"NVB06F_WFI", "NVB06F_FB_FLUSH", "NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY", "NON_STALL_INTERRUPT"})
        EXPECT_THAT(fence, HasSubstr(step));
}

TEST(RawInlineSignals, AReleaseWithNothingHostVisibleWrittenIsTheHostsReleaseAlone)
{
    uint32_t words[kEventReleaseWords];
    Push p{words, words + kEventReleaseWords};
    emitEventRelease(p, 0xff'fffe'0840ull, 9, false);
    ASSERT_EQ(p.at, words + 7);
    EXPECT_EQ(words[0], Push::hdr(1, 0, NVB06F_SEMAPHOREA, 4));
    EXPECT_EQ(words[3], 9u);
    EXPECT_EQ(words[4], NVB06F_SEMAPHORED_OPERATION_RELEASE | NVB06F_SEMAPHORED_RELEASE_SIZE_4BYTE << 24);
    EXPECT_EQ(words[5], Push::hdr(1, 0, NVB06F_NON_STALL_INTERRUPT, 1));
    const std::string map = Body(SourceText("../../TegraGM20B/SwitchGpuVmMap.h"), "int tgm_vm_kmap_raw(");
    EXPECT_THAT(map, HasSubstr(".vol = 1"));
}

TEST(RawInlineSignals, OnlyHostVisibleWritesSinceTheLastFlushMakeAReleaseFlush)
{
    EventPublication p;
    EXPECT_FALSE(p.take());
    p.wrote(false);
    EXPECT_FALSE(p.take());
    p.wrote(true);
    p.wrote(false);
    EXPECT_TRUE(p.take());
    EXPECT_FALSE(p.take());
    // Noted ahead of the use memo, which names an object once a command buffer however often it is written
    const std::string use = SourceText("../raw/runtime/raw_job_uses.cpp");
    EXPECT_THAT(use, HasSubstr("publication_.wrote(write && buffer.hostVisible);\n    if (named_.named(&buffer"));
    EXPECT_THAT(use, HasSubstr("publication_.wrote(range.hostVisible || (texture.import && !texture.import->id) || "
                               "texture.callerPages);\n    if (named_.named(&texture"));
}

TEST(RawInlineSignals, PayloadIsTheValueAboveTheBaseInsideTheWindow)
{
    EXPECT_EQ(eventReleasePayload(15, 10, 4), 5u);
    EXPECT_EQ(eventReleasePayload(14, 10, 4), 0u);
    EXPECT_EQ(eventReleasePayload(9, 10, 0), 0u);
    EXPECT_EQ(eventReleasePayload(10 + (uint64_t(1) << 31), 10, 0), 0u);
    EXPECT_EQ(eventReleasePayload(9 + (uint64_t(1) << 31), 10, 0), 0x7fffffffu);
    EXPECT_EQ(eventReleasePayload(3, ~uint64_t(0) - 4, 0), 8u);
}

}  // namespace
