// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "switch_host1x_submit_stubs.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <array>
#include <vector>
#include "bazel/testing/source_text.h"
#define SWITCH_DCACHE_OP(op, address) ((void)(address))
#define SWITCH_DCACHE_BARRIER()       ((void)0)
#include "../TegraHost1x.cpp"
#include "../SwitchHost1xChannels.cpp"

namespace {
alignas(16384) uint8_t gHostRegisters[kHost1xSize];
alignas(4096) uint8_t gMcRegisters[0x1000];
alignas(4096) uint8_t gPushBuffers[TEGRA_HOST1X_CHANNELS][SwitchHost1xPush::kBytes + 4];
alignas(4096) uint8_t gArenas[TEGRA_HOST1X_CHANNELS][SWITCH_HOST1X_ARENA_WORDS * 4];
uint32_t gAllocated;
std::vector<std::array<uint32_t, TEGRA_HOST1X_CHANNELS>> gDemands;  // every channel's DMAPUT at each demand
std::vector<switch_host1x_fence *> gFencesAdded;

constexpr uint64_t kPushPA = 0x80000000u, kArenaPA = 0x90000000u;

uint32_t doorbell(uint32_t channel)
{
    return switch_mmio_read32(gHostRegisters, channel * kChannelStride + CH_DMAPUT);
}

uint32_t &syncpoint(uint32_t id)
{
    return *reinterpret_cast<uint32_t *>(gHostRegisters + kSyncOffset + SYNC_SYNCPT + 4 * id);
}

const uint32_t *push(uint32_t channel)
{
    return reinterpret_cast<const uint32_t *>(gPushBuffers[channel]);
}

uint32_t arenaAddress(uint32_t channel, const uint32_t *words)
{
    return static_cast<uint32_t>(kArenaPA + channel * 0x100000u +
                                 (reinterpret_cast<const uint8_t *>(words) - gArenas[channel]));
}
}  // namespace

extern "C" uintptr_t ml_io_map(uintptr_t pa, uintptr_t)
{
    return reinterpret_cast<uintptr_t>(pa == kHost1xPA ? gHostRegisters : gMcRegisters);
}

extern "C" bool switch_car_clock_enabled(unsigned)
{
    return true;
}

extern "C" bool switch_car_reset_asserted(unsigned)
{
    return false;
}

extern "C" uint32_t switch_car_read(uint32_t)
{
    return SOURCE_PLLP | HOST1X_DIV_2_5;
}

extern "C" void switch_car_write(uint32_t, uint32_t)
{}

extern "C" void switch_car_clock_enable(unsigned)
{}

extern "C" void switch_car_reset_deassert(unsigned)
{}

// Each channel's push buffer, then its arena.
bool switch_dma_alloc(switch_dma_buffer *out, size_t size, uint64_t, IOOptionBits)
{
    const uint32_t channel = gAllocated / 2;
    const bool arena = gAllocated % 2;
    if (channel == TEGRA_HOST1X_CHANNELS || size != (arena ? sizeof(gArenas[0]) : sizeof(gPushBuffers[0])))
        return false;
    ++gAllocated;
    out->va = arena ? gArenas[channel] : gPushBuffers[channel];
    out->pa = (arena ? kArenaPA : kPushPA) + channel * 0x100000u;
    out->size = size;
    return true;
}

IOService *switch_interrupt_nub(uint32_t, bool)
{
    static IOService nub;
    return &nub;
}

void switch_interrupt_describe(uint32_t, char *buffer, size_t size)
{
    snprintf(buffer, size, "host test");
}

bool switch_host1x_intr_init(IOWorkLoop *, volatile uint8_t *, uint32_t)
{
    return true;
}

void switch_host1x_intr_start()
{}

void switch_host1x_intr_stop()
{}

void switch_host1x_intr_handle()
{}

void switch_host1x_intr_filter()
{}

extern "C" void switch_host1x_fence_init(switch_host1x_fence *fence, uint32_t syncpt, uint32_t threshold,
                                         void (*signaled)(switch_host1x_fence *, int))
{
    *fence = {};
    fence->syncpt = syncpt;
    fence->threshold = threshold;
    fence->signaled = signaled;
}

extern "C" int switch_host1x_fence_add(switch_host1x_fence *fence, uint32_t)
{
    gFencesAdded.push_back(fence);
    return TEGRA_HOST1X_OK;
}

extern "C" bool switch_host1x_fence_remove(switch_host1x_fence *)
{
    return true;
}

extern "C" void switch_emc_gpu_submit(void)
{
    std::array<uint32_t, TEGRA_HOST1X_CHANNELS> puts{};
    for (uint32_t channel = 0; channel < TEGRA_HOST1X_CHANNELS; channel++)
        puts[channel] = doorbell(channel);
    gDemands.push_back(puts);
}

namespace {
struct Host1xSubmit : testing::Test {
    void SetUp() override
    {
        ASSERT_EQ(tegra_host1x_start(), TEGRA_HOST1X_OK);
        IOLockLock(gSubmitLock);
        switch_host1x_channels_program();
        IOLockUnlock(gSubmitLock);
        gDemands.clear();
        gFencesAdded.clear();
        switch_test_sleep = nullptr;
    }

    // An engine's job: SETCLASS, `methods` method pairs, OP_DONE, built where host1x will read it.
    static const uint32_t *job(uint32_t channel, uint32_t methods, uint32_t threshold)
    {
        uint32_t *words = nullptr;
        EXPECT_EQ(switch_host1x_job_reserve(channel, 2 + 3 * methods, &words), TEGRA_HOST1X_OK);
        if (!words)
            return nullptr;
        uint32_t n = 0;
        words[n++] = tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_VIC, 0, 0);
        for (uint32_t i = 0; i < methods; ++i) {
            words[n++] = tegra_host1x_op_incr(0x10, 2);
            words[n++] = i;
            words[n++] = threshold;
        }
        words[n++] = tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, 170);
        EXPECT_EQ(switch_host1x_job_submit(channel, words, n, 170, threshold), TEGRA_HOST1X_OK);
        return words;
    }
};

TEST_F(Host1xSubmit, EveryEngineRaisesTheMemoryClockBeforeItsDoorbell)
{
    const uint32_t words[] = {tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_NVDEC, 0, 0),
                              tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_IMMEDIATE, 170),
                              tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, 170)};

    const struct {
        const char *engine;
        uint32_t channel;
    } engines[] = {{"NVDEC", TEGRA_HOST1X_CHANNEL_DEFAULT},
                   {"VIC", TEGRA_HOST1X_CHANNEL_VIC},
                   {"NVENC", TEGRA_HOST1X_CHANNEL_NVENC},
                   {"NVJPG", TEGRA_HOST1X_CHANNEL_NVJPG}};

    for (const auto &e : engines) {
        for (const bool copied : {true, false}) {
            gDemands.clear();
            const uint32_t before = doorbell(e.channel);
            if (copied)
                ASSERT_EQ(e.channel == TEGRA_HOST1X_CHANNEL_DEFAULT ? tegra_host1x_submit(words, 3)
                                                                    : tegra_host1x_submit_on(e.channel, words, 3),
                          TEGRA_HOST1X_OK)
                    << e.engine;
            else
                ASSERT_NE(job(e.channel, 1, 1), nullptr) << e.engine;
            EXPECT_EQ(doorbell(e.channel), before + 8) << e.engine << ": one GATHER and its address";
            ASSERT_EQ(gDemands.size(), 1u) << e.engine << ": a submission registers EMC demand once";
            EXPECT_EQ(gDemands[0][e.channel], before) << e.engine << ": demand before the doorbell";
        }
    }
    gDemands.clear();
    EXPECT_EQ(tegra_host1x_submit_on(TEGRA_HOST1X_CHANNELS, words, 3), TEGRA_HOST1X_EINVAL);
    EXPECT_TRUE(gDemands.empty());
}

TEST_F(Host1xSubmit, ThePushBufferHoldsOnlyGatherPairsOfTheWordsTheEngineWrote)
{
    constexpr uint32_t channel = TEGRA_HOST1X_CHANNEL_VIC;
    syncpoint(170) = 0;
    std::vector<std::pair<uint32_t, uint32_t>> expected(SwitchHost1xPush::kSlots);
    for (uint32_t i = 1; i <= SwitchHost1xPush::kSlots + 40; ++i) {
        syncpoint(170) = i > 8 ? i - 8 : 0;
        const uint32_t slot = doorbell(channel) / 8;
        const uint32_t methods = 1 + i % 60;
        const uint32_t *words = job(channel, methods, i);
        ASSERT_NE(words, nullptr) << i;
        expected[slot] = {SwitchHost1xPush::gather(2 + 3 * methods), arenaAddress(channel, words)};
    }
    for (uint32_t slot = 0; slot < SwitchHost1xPush::kSlots; ++slot) {
        EXPECT_EQ(push(channel)[2 * slot], expected[slot].first) << "slot " << slot;
        EXPECT_EQ(push(channel)[2 * slot + 1], expected[slot].second) << "slot " << slot;
        EXPECT_EQ(push(channel)[2 * slot] >> 28, 6u) << "slot " << slot << ": no NOP padding, no copied stream";
    }
    EXPECT_EQ(push(channel)[SwitchHost1xPush::kBytes / 4], SwitchHost1xPush::restart(0));
    EXPECT_EQ(switch_mmio_read32(gHostRegisters, channel * kChannelStride + CH_DMAEND), SwitchHost1xPush::kBytes + 4);
    EXPECT_EQ(switch_mmio_read32(gHostRegisters, channel * kChannelStride + CH_DMASTART),
              kPushPA + channel * 0x100000u);
}

TEST_F(Host1xSubmit, TheGatherFilterIsClearedOnEveryChannel)
{
    for (uint32_t channel = 0; channel < TEGRA_HOST1X_CHANNELS; ++channel)
        switch_mmio_write32(gHostRegisters, channel * kChannelStride + CH_CHANNELCTRL, 0x5);
    IOLockLock(gSubmitLock);
    switch_host1x_channels_program();
    IOLockUnlock(gSubmitLock);
    for (uint32_t channel = 0; channel < TEGRA_HOST1X_CHANNELS; ++channel)
        EXPECT_EQ(switch_mmio_read32(gHostRegisters, channel * kChannelStride + CH_CHANNELCTRL), 0x1u) << channel;
    EXPECT_EQ(switch_host1x_channels_filter_stuck(), 0u);
}

TEST_F(Host1xSubmit, AProofOfLifeIsCopiedWithItsFenceAndGivesItsRoomBack)
{
    constexpr uint32_t channel = TEGRA_HOST1X_CHANNEL_NVJPG;
    syncpoint(170) = 0;
    const uint32_t words[] = {tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_NVJPG, 0, 0),
                              tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_IMMEDIATE, 170),
                              tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, 170)};
    for (uint32_t i = 0; i < 2 * SwitchHost1xArena::kEntries; ++i) {
        ASSERT_EQ(switch_host1x_job_copy(channel, words, 3, 170, 2 * i + 2), TEGRA_HOST1X_OK) << i;
        syncpoint(170) = 2 * i + 2;
    }
    for (uint32_t i = 0; i < SwitchHost1xArena::kEntries; ++i)
        ASSERT_EQ(tegra_host1x_submit_on(channel, words, 3), TEGRA_HOST1X_OK) << i;
    EXPECT_EQ(tegra_host1x_submit_on(channel, words, 3), TEGRA_HOST1X_ENOMEM)
        << "streams without a fence hold their room until a fenced job passes";
}

TEST_F(Host1xSubmit, AJobsArenaRoomComesBackOnlyOnceItsFencePasses)
{
    constexpr uint32_t channel = TEGRA_HOST1X_CHANNEL_NVENC, big = SWITCH_HOST1X_ARENA_WORDS / 2;
    syncpoint(170) = 100;
    uint32_t *words = nullptr;
    for (const uint32_t threshold : {101u, 102u}) {
        ASSERT_EQ(switch_host1x_job_reserve(channel, big, &words), TEGRA_HOST1X_OK);
        words[0] = tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_NVENC, 0, 0);
        ASSERT_EQ(switch_host1x_job_submit(channel, words, big, 170, threshold), TEGRA_HOST1X_OK);
    }
    EXPECT_EQ(switch_host1x_job_reserve(channel, 1, &words), TEGRA_HOST1X_EAGAIN);
    EXPECT_EQ(tegra_host1x_submit_on(channel, words, 1), TEGRA_HOST1X_ENOMEM) << "a copied stream never sleeps";

    // The submitter sleeps on the oldest job's fence; the fence passing wakes it with the room back.
    int sleeps = 0;
    switch_test_sleep = [&] {
        ++sleeps;
        ASSERT_EQ(gFencesAdded.size(), 1u);
        EXPECT_EQ(gFencesAdded[0]->threshold, 101u);
        EXPECT_FALSE(gSubmitLock->held);
        syncpoint(170) = 101;
        gFencesAdded[0]->signaled(gFencesAdded[0], 0);
    };
    EXPECT_EQ(switch_host1x_job_wait_room(channel, big), TEGRA_HOST1X_OK);
    EXPECT_EQ(sleeps, 1);
    ASSERT_EQ(switch_host1x_job_reserve(channel, big, &words), TEGRA_HOST1X_OK);
    EXPECT_EQ(arenaAddress(channel, words), kArenaPA + channel * 0x100000u) << "the freed room is the arena's start";
    switch_host1x_job_cancel(channel);
}

TEST_F(Host1xSubmit, AResetOrRestartedChannelFetchesNothingOldAndHoldsNoRoom)
{
    constexpr uint32_t channel = TEGRA_HOST1X_CHANNEL_NVENC;
    syncpoint(170) = 0;
    uint32_t *words = nullptr;
    ASSERT_EQ(switch_host1x_job_reserve(channel, SWITCH_HOST1X_ARENA_WORDS, &words), TEGRA_HOST1X_OK);
    ASSERT_EQ(switch_host1x_job_submit(channel, words, SWITCH_HOST1X_ARENA_WORDS, 170, 5), TEGRA_HOST1X_OK);
    EXPECT_EQ(switch_host1x_job_reserve(channel, 1, &words), TEGRA_HOST1X_EAGAIN);
    tegra_host1x_channel_teardown(channel);
    tegra_host1x_channel_restart(channel);
    EXPECT_EQ(switch_host1x_job_reserve(channel, SWITCH_HOST1X_ARENA_WORDS, &words), TEGRA_HOST1X_OK);
    switch_host1x_job_cancel(channel);

    ASSERT_EQ(switch_host1x_job_reserve(channel, SWITCH_HOST1X_ARENA_WORDS, &words), TEGRA_HOST1X_OK);
    ASSERT_EQ(switch_host1x_job_submit(channel, words, SWITCH_HOST1X_ARENA_WORDS, 170, 6), TEGRA_HOST1X_OK);
    syncpoint(170) = 6;
    tegra_host1x_suspend();
    EXPECT_EQ(switch_host1x_job_reserve(channel, 1, &words), TEGRA_HOST1X_ENODEV);
    EXPECT_EQ(tegra_host1x_submit_on(channel, words, 1), TEGRA_HOST1X_ENODEV);
    tegra_host1x_resume();
    EXPECT_EQ(doorbell(channel), 0u);
    EXPECT_EQ(switch_mmio_read32(gHostRegisters, channel * kChannelStride + CH_DMASTART),
              kPushPA + channel * 0x100000u);
    EXPECT_EQ(switch_host1x_job_reserve(channel, SWITCH_HOST1X_ARENA_WORDS, &words), TEGRA_HOST1X_OK);
    switch_host1x_job_cancel(channel);
}

TEST_F(Host1xSubmit, AStreamOutsideItsReservationIsRefusedAndEndsIt)
{
    uint32_t *words = nullptr;
    ASSERT_EQ(switch_host1x_job_reserve(TEGRA_HOST1X_CHANNEL_VIC, 10, &words), TEGRA_HOST1X_OK);
    EXPECT_EQ(switch_host1x_job_reserve(TEGRA_HOST1X_CHANNEL_VIC, 10, &words), TEGRA_HOST1X_EAGAIN);
    const uint32_t before = doorbell(TEGRA_HOST1X_CHANNEL_VIC);
    EXPECT_EQ(switch_host1x_job_submit(TEGRA_HOST1X_CHANNEL_VIC, words, 11, 170, 1), TEGRA_HOST1X_EINVAL);
    EXPECT_EQ(doorbell(TEGRA_HOST1X_CHANNEL_VIC), before);
    EXPECT_EQ(switch_host1x_job_reserve(TEGRA_HOST1X_CHANNEL_VIC, 10, &words), TEGRA_HOST1X_OK);
    switch_host1x_job_cancel(TEGRA_HOST1X_CHANNEL_VIC);
    EXPECT_EQ(switch_host1x_job_submit(TEGRA_HOST1X_CHANNEL_VIC, words, 1, 170, 1), TEGRA_HOST1X_EINVAL)
        << "no reservation open";
}

// Each engine's job is built where host1x gathers it: its job path reserves arena room, writes its stream there and
// submits it with its fence; only its one-off proof of life is copied, with its fence too.
TEST(Host1xSubmitShape, EveryEngineBuildsItsJobsInTheArena)
{
    using switch_ios::test::Body;
    using switch_ios::test::SourceText;

    const struct {
        const char *source, *function, *reserve, *submit;
    } engines[] = {
        {"../../TegraNVDEC/SwitchNVDECJobs.cpp", "int publish(",
         "switch_host1x_job_reserve(TEGRA_HOST1X_CHANNEL_DEFAULT, capacity, &work.words)",
         "switch_host1x_job_submit(TEGRA_HOST1X_CHANNEL_DEFAULT, work.words, n, gSyncpt,"},
        {"../../TegraVIC/SwitchVICJobs.cpp", "int switch_vic_submit_checked(",
         "switch_host1x_job_reserve(TEGRA_HOST1X_CHANNEL_VIC, capacity, &words)",
         "switch_host1x_job_submit(TEGRA_HOST1X_CHANNEL_VIC, words, n, gSyncpt,"},
        {"../../TegraNVENC/TegraNVENC.cpp", "int run(",
         "switch_host1x_job_begin(TEGRA_HOST1X_CHANNEL_NVENC, kWords, &words)",
         "switch_host1x_job_submit(TEGRA_HOST1X_CHANNEL_NVENC, words + first, n - first, gSyncpt, gBase + job)"},
        {"../../TegraNVJPG/TegraNVJPG.cpp", "int tegra_nvjpg_run(",
         "switch_host1x_job_begin(TEGRA_HOST1X_CHANNEL_NVJPG, 1 + 3 * 11 + 1, &words)",
         "switch_host1x_job_submit(TEGRA_HOST1X_CHANNEL_NVJPG, words, n, gSyncpt, fence)"},
    };

    for (const auto &e : engines) {
        const std::string body = Body(SourceText(e.source), e.function);
        EXPECT_THAT(body, testing::HasSubstr(e.reserve)) << e.source;
        EXPECT_THAT(body, testing::HasSubstr(e.submit)) << e.source;
        EXPECT_THAT(body, testing::Not(testing::HasSubstr("switch_host1x_job_copy"))) << e.source << ": no copy";
        EXPECT_THAT(body, testing::Not(testing::HasSubstr("uint32_t words["))) << e.source << ": no staging array";
    }
    for (const char *source :
         {"../../TegraNVDEC/SwitchNVDECJobs.cpp", "../../TegraNVDEC/TegraNVDEC.cpp", "../../TegraVIC/SwitchVICJobs.cpp",
          "../../TegraVIC/TegraVIC.cpp", "../../TegraNVENC/TegraNVENC.cpp", "../../TegraNVJPG/TegraNVJPG.cpp"})
        EXPECT_THAT(SourceText(source), testing::Not(testing::HasSubstr("tegra_host1x_submit")))
            << source << ": every stream has its fence";
}
}  // namespace
