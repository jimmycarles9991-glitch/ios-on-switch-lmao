// SPDX-License-Identifier: MIT
// raw_stamps.h: a raw command buffer's GPU time from the two 16-byte semaphore releases its push writes, in nanoseconds of a
// PTIMER that counts 32 a tick of the 19.2 MHz oscillator; and the guards that keep its last release from waiting longer than
// the kernel's own end of the submission does.
#include <cstdint>
#include <cstring>
#include <string>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_stamps.h"

namespace {
using namespace raw;
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

TEST(RawStamps, TheReleaseIsTheKernelsStatsStamp)
{
    uint32_t words[kStampWords];
    Push p{words, words + kStampWords};
    emitStamp(p, 0x12'3456'7890ull, 77);
    ASSERT_EQ(p.at, words + kStampWords);
    EXPECT_EQ(words[0], Push::hdr(1, 0, NV906F_SEMAPHOREA, 4));
    EXPECT_EQ(words[1], 0x12u);
    EXPECT_EQ(words[2], 0x34567890u);
    EXPECT_EQ(words[3], 77u);
    // SEMAPHORED: OPERATION 3:0, RELEASE_WFI 20:20, RELEASE_SIZE 24:24 (cl906f.h)
    EXPECT_EQ(words[4], NV906F_SEMAPHORED_OPERATION_RELEASE | NV906F_SEMAPHORED_RELEASE_WFI_EN << 20 |
                            NV906F_SEMAPHORED_RELEASE_SIZE_16BYTE << 24);
}

// What the GPU does with one release: payload and PTIMER at the semaphore's address, here a page standing in for the stamps
void release(const uint32_t (&words)[kStampWords], uint64_t pageVa, uint8_t *page, uint64_t ptimer)
{
    const uint64_t at = (uint64_t(words[1]) << 32 | words[2]) - pageVa;
    ASSERT_LE(at + sizeof(StampRelease), 0x1000u);
    const StampRelease r{words[3], 0, ptimer};
    memcpy(page + at, &r, sizeof r);
}

// A command buffer's start and end releases, as begin() and commit() emit them, time it: both on the slot's start would
// leave its end unwritten and every command buffer unstamped
TEST(RawStamps, ACommandBuffersTwoReleasesTimeIt)
{
    const uint64_t pageVa = 0xc0'0000'0000ull;
    alignas(StampSlot) uint8_t page[0x1000] = {};
    for (const uint32_t stamp : {1u, 2u, kStampSlots, kStampSlots + 5}) {
        uint32_t start[kStampWords], end[kStampWords];
        Push ps{start, start + kStampWords}, pe{end, end + kStampWords};
        emitStartStamp(ps, pageVa, stamp);
        emitEndStamp(pe, pageVa, stamp);
        release(start, pageVa, page, 1'000'000);
        release(end, pageVa, page, 1'000'000 + 614'400);
        const auto &slot = *reinterpret_cast<const StampSlot *>(page + stampOffset(stamp));
        uint64_t ns = 0;
        ASSERT_TRUE(stampedNanoseconds(slot, stamp, ns)) << stamp;
        EXPECT_EQ(ns, 1'000'000u) << stamp;
    }
    const std::string raw = SourceText("../raw/runtime/raw_gpu.cpp");
    EXPECT_THAT(Body(raw, "CommandBuffer *Device::begin()"), HasSubstr("emitStartStamp(p, stamps_.va, cb.stamp);"));
    EXPECT_THAT(Body(raw, "bool CommandBuffer::commit(uint32_t &seqno)"),
                HasSubstr("emitEndStamp(p, d.stamps_.va, stamp);"));
}

// A measured draw's two releases (raw_band_rates.h): 3D report semaphores at the end of the pipeline, after the earlier
// draws' writes, writing PTIMER, never stalling the front end; each in its own slot past the command buffers' page
TEST(RawStamps, AMeasuredDrawsReleasesTimeIt)
{
    uint32_t start[kDrawStampWords], end[kDrawStampWords];
    Push ps{start, start + kDrawStampWords}, pe{end, end + kDrawStampWords};
    const uint64_t pageVa = 0xc0'0000'0000ull;
    emitDrawStamp(ps, pageVa, kDrawStampSlots + 3, false, 41);
    emitDrawStamp(pe, pageVa, kDrawStampSlots + 3, true, 41);
    ASSERT_EQ(ps.at, start + kDrawStampWords);
    EXPECT_EQ(start[0], Push::hdr(1, 0, NVB197_SET_REPORT_SEMAPHORE_A, 4));
    EXPECT_EQ(uint64_t(start[1]) << 32 | start[2], pageVa + kStampSlots * sizeof(StampSlot) + 3 * sizeof(StampSlot));
    EXPECT_EQ(uint64_t(end[1]) << 32 | end[2], pageVa + kStampSlots * sizeof(StampSlot) + 3 * sizeof(StampSlot) + 16);
    EXPECT_EQ(start[3], 41u);
    // OPERATION 1:0 RELEASE, RELEASE 4:4 after writes, FLUSH_DISABLE 2:2, PIPELINE_LOCATION 15:12 ALL, FOUR_WORDS 28:28
    EXPECT_EQ(start[4], 1u << 4 | 1u << 2 | 0xfu << 12);
    EXPECT_EQ(end[4], start[4]);
    alignas(StampSlot) uint8_t page[kStampBytes] = {};
    const StampRelease a{41, 0, 5'000}, b{41, 0, 5'000 + 6'144};
    memcpy(page + drawStampOffset(3, false), &a, sizeof a);
    memcpy(page + drawStampOffset(3, true), &b, sizeof b);
    uint64_t ns = 0;
    ASSERT_TRUE(stampedNanoseconds(*reinterpret_cast<const StampSlot *>(page + drawStampOffset(3, false)), 41, ns));
    EXPECT_EQ(ns, 10'000u);
    EXPECT_FALSE(stampedNanoseconds(*reinterpret_cast<const StampSlot *>(page + drawStampOffset(3, false)), 40, ns));
}

TEST(RawStamps, OneSecondOfTheOscillatorIsOneSecond)
{
    EXPECT_EQ(31'250'000ull * kPtimerNsDen, 19'200'000ull * kPtimerNsNum);
    EXPECT_EQ(ptimerNanoseconds(19'200'000ull * 32), 1'000'000'000ull);
    EXPECT_EQ(ptimerNanoseconds(384), 625u);
    EXPECT_EQ(ptimerNanoseconds(1), 1u);
    // a day of PTIMER, far past any command buffer, without overflow
    const uint64_t day = 86'400ull * 19'200'000 * 32;
    EXPECT_EQ(ptimerNanoseconds(day), 86'400'000'000'000ull);
}

TEST(RawStamps, AStampedSlotGivesItsGpuTime)
{
    StampSlot slot{{9, 0, 1'000'000}, {9, 0, 1'000'000 + 614'400}};
    uint64_t ns = 0;
    ASSERT_TRUE(stampedNanoseconds(slot, 9, ns));
    EXPECT_EQ(ns, 1'000'000u);
}

TEST(RawStamps, ASlotAnotherCommandBufferWroteGivesNone)
{
    uint64_t ns = 7;
    const StampSlot later{{9 + kStampSlots, 0, 10}, {9 + kStampSlots, 0, 20}};
    EXPECT_FALSE(stampedNanoseconds(later, 9, ns));
    const StampSlot startedOver{{9 + kStampSlots, 0, 10}, {9, 0, 20}};
    EXPECT_FALSE(stampedNanoseconds(startedOver, 9, ns));
    const StampSlot unwritten{};
    EXPECT_FALSE(stampedNanoseconds(unwritten, 0, ns));
    EXPECT_FALSE(stampedNanoseconds(unwritten, 9, ns));
    EXPECT_EQ(ns, 7u);
}

TEST(RawStamps, StampsAreNeverZeroAndShareNoSlotInFlight)
{
    EXPECT_EQ(stampOf(0), 1u);
    EXPECT_EQ(stampOf(0xfffffffeull), 0xffffffffu);
    EXPECT_EQ(stampOf(0xffffffffull), 1u);
    for (uint64_t c = 0; c < kStampSlots; c++)
        for (uint64_t d = c + 1; d < kStampSlots; d++)
            ASSERT_NE(stampOffset(stampOf(c)), stampOffset(stampOf(d)));
    EXPECT_LE(stampOffset(stampOf(kStampSlots - 1)) + sizeof(StampSlot), 0x1000u);
}

void expectMetalsOrder(const CommandBufferTimes &t)
{
    EXPECT_LE(t.kernelStart, t.kernelEnd);
    EXPECT_LE(t.kernelEnd, t.gpuStart);
    EXPECT_LE(t.gpuStart, t.gpuEnd);
}

TEST(RawStamps, TimesKeepMetalsOrderAndTheGpusDuration)
{
    const CommandBufferTimes inside = orderedTimes(1.0, 1.001, 1.010, true, 5'000'000);
    expectMetalsOrder(inside);
    EXPECT_DOUBLE_EQ(inside.kernelStart, 1.0);
    EXPECT_DOUBLE_EQ(inside.kernelEnd, 1.001);
    EXPECT_NEAR(inside.gpuEnd - inside.gpuStart, 0.005, 1e-12);
    // the GPU started before the submitting thread saw its submission return, or (late completion) before the commit
    for (const uint64_t ns : {9'500'000ull, 20'000'000ull}) {
        const CommandBufferTimes early = orderedTimes(1.0, 1.002, 1.010, true, ns);
        expectMetalsOrder(early);
        EXPECT_NEAR(early.gpuEnd - early.gpuStart, double(ns) * 1e-9, 1e-12);
    }
}

TEST(RawStamps, UnstampedTimesAreTheHostsWindow)
{
    const CommandBufferTimes window = orderedTimes(1.0, 1.001, 1.010, false, 0);
    expectMetalsOrder(window);
    EXPECT_DOUBLE_EQ(window.gpuStart, 1.001);
    EXPECT_DOUBLE_EQ(window.gpuEnd, 1.010);
    // scheduled by its completion (its fence passed first, or it never reached a GPU)
    expectMetalsOrder(orderedTimes(1.0, 1.010, 1.010, false, 0));
}

// The last release waits for idle (RELEASE_WFI EN). It adds no wait while the kernel follows every user push with its own wait
// for idle and a fence release that waits too, and it is the command buffer's last method, after the tiled cache's flush
TEST(RawStamps, TheEndReleaseWaitsNoLongerThanTheKernelsFence)
{
    const std::string gpu = SourceText("../../TegraGM20B/gpu_inline.h") + SourceText("../../TegraGM20B/gpu_exec.c");
    EXPECT_THAT(Body(gpu, "static inline int l2_on(void)"), HasSubstr("return 1;"));
    // A submission without explicit visibility (every one this runtime makes) ends with the external fence.
    EXPECT_THAT(Body(gpu, "static inline u32 chan_fence(struct tgm_chan *chan)"),
                HasSubstr("return chan_fence_visibility(chan, 1);"));
    EXPECT_THAT(gpu, HasSubstr("int external = !(visibility & SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT) ||"));
    const std::string fence =
        Body(gpu, "static u32 chan_fence_job(struct tgm_chan *chan, int external, u32 first, u32 next, int quiet");
    EXPECT_THAT(fence, HasSubstr("PUSH_NVSQ(push, NV906F, NVB06F_WFI, 0);"));
    EXPECT_THAT(fence, HasSubstr("NVDEF(NV906F, SEMAPHORED, RELEASE_WFI, EN)"));
    const std::string raw = SourceText("../raw/runtime/raw_gpu.cpp");
    const std::string commit = Body(raw, "bool CommandBuffer::commit(uint32_t &seqno)");
    const size_t flush = commit.find("tiledCacheFlush(p.at)"), stamp = commit.find("emitEndStamp(");
    ASSERT_NE(flush, std::string::npos);
    ASSERT_NE(stamp, std::string::npos);
    EXPECT_LT(flush, stamp);
    EXPECT_THAT(Body(raw, "CommandBuffer *Device::begin()"), HasSubstr("emitStartStamp(p, stamps_.va, cb.stamp)"));
}

}  // namespace
