// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "switch_host1x_submit_stubs.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <array>
#include <vector>
#include "bazel/testing/source_text.h"
#define SWITCH_MMIO_HOST_MODEL        1
#define SWITCH_DCACHE_OP(op, address) ((void)(address))
#define SWITCH_DCACHE_BARRIER()       ((void)0)
#include "../../../kits/SwitchMMIO/SwitchMMIO.h"

// The host model's publishing write is a plain store, so the drivers' publishing writes are counted here instead.
void switch_test_publish(volatile uint8_t *base, uint32_t offset, uint32_t value);
#define switch_mmio_write32_publish switch_test_publish
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
std::vector<std::pair<uint32_t, uint32_t>> gRegisterWrites;
bool gCpuIncrements, gPublishing;
uint32_t gStuckSyncpt;                   // a syncpoint whose CPU increments do not take
std::vector<uint32_t> gPlainIncrements;  // CPU increments written without ordering the stores before them
uint32_t gDisplayTeardowns;
uint32_t gFailedProducer;
int gProducerPhysical;

struct FailedNode {
    uint32_t engine, syncpt, source;
    int physical;
};

std::vector<FailedNode> gFailedNodes;
uint32_t gFailurePasses;

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

extern "C" uint32_t switch_mmio_model_read32(uintptr_t address)
{
    return *reinterpret_cast<uint32_t *>(address);
}

extern "C" void switch_mmio_model_write32(uintptr_t address, uint32_t bits)
{
    *reinterpret_cast<uint32_t *>(address) = bits;
    const uintptr_t from = reinterpret_cast<uintptr_t>(gHostRegisters);
    if (address < from || address - from >= sizeof(gHostRegisters))
        return;
    const uint32_t offset = uint32_t(address - from);
    gRegisterWrites.emplace_back(offset, bits);
    if (offset == kSyncOffset + SYNC_CH_TEARDOWN && (bits & (1u << TEGRA_HOST1X_CHANNEL_DISPLAY)))
        ++gDisplayTeardowns;
    const bool increment = offset >= kSyncOffset + 0xf60 && offset < kSyncOffset + 0xf78;
    if (increment && !gPublishing)
        gPlainIncrements.push_back(bits);
    if (gCpuIncrements && increment) {
        const uint32_t first = (offset - kSyncOffset - 0xf60) / 4 * 32;
        for (uint32_t bit = 0; bit < 32; ++bit)
            if ((bits & (1u << bit)) && first + bit != gStuckSyncpt)
                ++syncpoint(first + bit);
    }
}

void switch_test_publish(volatile uint8_t *base, uint32_t offset, uint32_t value)
{
    gPublishing = true;
    switch_mmio_write32(base, offset, value);
    gPublishing = false;
}

extern "C" int switch_gpu_backing_token_status(const switch_gpu_backing_token *)
{
    return 1;
}

extern "C" int switch_gpu_backing_token_physical_status(const switch_gpu_backing_token *token, int)
{
    for (const auto &node : gFailedNodes)
        if (node.engine == token->engine && node.syncpt == token->queue)
            return node.physical;
    return token->queue == gFailedProducer ? gProducerPhysical : 0;
}

namespace {
bool failNodes(uint32_t engine, uint32_t syncpt, uint32_t reached)
{
    EXPECT_FALSE(gSubmitLock->held);
    bool changed = false;
    for (auto &node : gFailedNodes) {
        if (node.engine != engine || node.physical < 0)
            continue;
        bool failed = node.source == syncpt && !reached;
        for (const auto &source : gFailedNodes)
            failed |= source.syncpt == node.source && source.physical < 0;
        if (failed) {
            node.physical = -5;
            changed = true;
        }
    }
    return changed;
}
}  // namespace

extern "C" bool switch_nvdec_host1x_cancelled(uint32_t syncpt, uint32_t reached)
{
    ++gFailurePasses;
    return failNodes(SWITCH_GPU_ENGINE_NVDEC, syncpt, reached);
}

extern "C" bool switch_vic_host1x_cancelled(uint32_t syncpt, uint32_t reached)
{
    bool changed = failNodes(SWITCH_GPU_ENGINE_VIC, syncpt, reached);
    if (syncpt == gFailedProducer && gProducerPhysical >= 0) {
        gProducerPhysical = -5;
        changed = true;
    }
    return changed;
}

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

extern "C" void tegra_mc_hot_reset(uint32_t, uint32_t, bool)
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
        gRegisterWrites.clear();
        gDisplayStream = nullptr;
        gCpuIncrements = gPublishing = false;
        gStuckSyncpt = 0;
        gPlainIncrements.clear();
        gDisplayTeardowns = 0;
        gFailedProducer = 0;
        gProducerPhysical = 0;
        gFailedNodes.clear();
        gFailurePasses = 0;
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
                   {"NVJPG", TEGRA_HOST1X_CHANNEL_NVJPG},
                   {"DC", TEGRA_HOST1X_CHANNEL_DISPLAY}};

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

// The display's class 0x70 streams have their own channel: its gathers ring only its doorbell, and its teardown stops
// and drops only its own jobs, never one an engine has queued.
TEST_F(Host1xSubmit, TheDisplaysChannelIsItsOwnAndItsTeardownDropsNoEnginesJob)
{
    constexpr uint32_t display = TEGRA_HOST1X_CHANNEL_DISPLAY, nvjpg = TEGRA_HOST1X_CHANNEL_NVJPG;
    syncpoint(170) = 0;
    uint32_t *words = nullptr;
    ASSERT_EQ(switch_host1x_job_reserve(nvjpg, SWITCH_HOST1X_ARENA_WORDS, &words), TEGRA_HOST1X_OK);
    words[0] = tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_NVJPG, 0, 0);
    ASSERT_EQ(switch_host1x_job_submit(nvjpg, words, SWITCH_HOST1X_ARENA_WORDS, 170, 5), TEGRA_HOST1X_OK);
    const uint32_t nvjpgPut = doorbell(nvjpg);

    ASSERT_EQ(switch_host1x_job_reserve(display, SWITCH_HOST1X_ARENA_WORDS, &words), TEGRA_HOST1X_OK);
    words[0] = tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_DC, 0, 0);
    ASSERT_EQ(switch_host1x_job_submit(display, words, SWITCH_HOST1X_ARENA_WORDS, 171, 1), TEGRA_HOST1X_OK);
    EXPECT_EQ(doorbell(display), 8u);
    EXPECT_EQ(doorbell(nvjpg), nvjpgPut);
    EXPECT_EQ(switch_mmio_read32(gHostRegisters, display * kChannelStride + CH_DMASTART),
              kPushPA + display * 0x100000u);
    EXPECT_EQ(switch_host1x_job_reserve(display, 1, &words), TEGRA_HOST1X_EAGAIN);

    tegra_host1x_channel_teardown(display);
    EXPECT_EQ(switch_mmio_read32(gHostRegisters, kSyncOffset + SYNC_CMDPROC_STOP), 1u << display);
    EXPECT_EQ(switch_mmio_read32(gHostRegisters, kSyncOffset + SYNC_CH_TEARDOWN), 1u << display);
    EXPECT_EQ(switch_mmio_read32(gHostRegisters, display * kChannelStride + CH_DMACTRL), DMACTRL_STOP);
    EXPECT_EQ(switch_mmio_read32(gHostRegisters, nvjpg * kChannelStride + CH_DMACTRL), 0u);
    tegra_host1x_channel_restart(display);
    EXPECT_EQ(switch_mmio_read32(gHostRegisters, kSyncOffset + SYNC_CMDPROC_STOP), 0u);
    EXPECT_EQ(doorbell(nvjpg), nvjpgPut);
    EXPECT_EQ(switch_host1x_job_reserve(nvjpg, 1, &words), TEGRA_HOST1X_EAGAIN) << "NVJPG's job still holds its room";
    EXPECT_EQ(switch_host1x_job_reserve(display, SWITCH_HOST1X_ARENA_WORDS, &words), TEGRA_HOST1X_OK)
        << "the display's dropped job gave its room back";
    switch_host1x_job_cancel(display);
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
// MOD_TEARDOWN's other bits write as NO_ACTION (TRM 14.9.14): a module's teardown is its bit alone, no read-modify-write.
TEST_F(Host1xSubmit, AModuleTeardownWritesThatModulesBitAlone)
{
    uint32_t &teardown = *reinterpret_cast<uint32_t *>(gHostRegisters + kSyncOffset + 0xb4);
    teardown = 0xffffffffu;
    tegra_host1x_module_teardown(TEGRA_HOST1X_MODULE_NVJPG);
    EXPECT_EQ(teardown, 1u << 14);
    tegra_host1x_module_teardown(TEGRA_HOST1X_MODULE_VIC);
    EXPECT_EQ(teardown, 1u << 13);
}

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

struct DisplayExecution {
    const uint32_t *commands = reinterpret_cast<const uint32_t *>(gArenas[TEGRA_HOST1X_CHANNEL_DISPLAY]);
    uint32_t pc = 0, teardown = gDisplayTeardowns;
    std::vector<std::pair<uint32_t, uint32_t>> controller;

    void run()
    {
        while (teardown == gDisplayTeardowns) {
            const uint32_t word = commands[pc];
            if (word == tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 0x4e, 5)) {
                if (static_cast<int32_t>(syncpoint(commands[pc + 2]) - commands[pc + 1]) < 0)
                    return;
                pc += 3;
            } else if (word == tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_DC, 0, 0)) {
                ++pc;
            } else if (word >> 28 == 1) {
                controller.emplace_back((word >> 16) & 0xfff, commands[pc + 1]);
                pc += 2;
            } else if (word >> 28 == 4) {
                ++syncpoint(word & 0xff);
                ++pc;
                return;
            } else {
                throw std::runtime_error("unsupported controller stream opcode");
            }
        }
    }
};

struct DisplayCompletion {
    uint32_t calls = 0;
    int status = 1;

    static void heard(void *context, int status)
    {
        auto &done = *static_cast<DisplayCompletion *>(context);
        EXPECT_FALSE(gSubmitLock->held);
        ++done.calls;
        done.status = status;
    }
};

TEST_F(Host1xSubmit, DisplayStreamWaitsThenWritesAndCompletesOnlyOnItsOwnSyncpoint)
{
    SwitchHost1xDisplayStream stream;
    uint32_t producer = 0;
    ASSERT_EQ(tegra_host1x_syncpt_alloc(&producer), 0);
    ASSERT_EQ(switch_host1x_display_start(&stream), 0);
    const switch_gpu_backing_token token = {1, 1, SWITCH_GPU_ENGINE_VIC, producer};
    const uint32_t words[2] = {0xdc0, 0xdc2}, values[2] = {0x20000000, 0x21000000};
    DisplayCompletion done;
    ASSERT_EQ(
        switch_host1x_display_submit(&stream, &token, 1, words, values, 2, 0x500, 5, DisplayCompletion::heard, &done),
        0);
    ASSERT_EQ(gFencesAdded.size(), 1u);
    EXPECT_EQ(gFencesAdded[0]->syncpt, stream.syncpt);
    EXPECT_EQ(done.calls, 0u);
    DisplayExecution execution;
    execution.run();
    EXPECT_TRUE(execution.controller.empty());
    syncpoint(producer) = 1;
    EXPECT_EQ(done.calls, 0u);
    execution.run();
    EXPECT_EQ(execution.controller, (std::vector<std::pair<uint32_t, uint32_t>>{
                                        {words[0], values[0]}, {words[1], values[1]}, {0x041, 0x500}, {0x041, 5}}));
    EXPECT_EQ(syncpoint(stream.syncpt), stream.fence.threshold);
    stream.fence.signaled(&stream.fence, 0);
    EXPECT_EQ(done.calls, 1u);
    EXPECT_EQ(done.status, 0);
    EXPECT_EQ(gDisplayStream, nullptr);
}

TEST_F(Host1xSubmit, AFailedProducerTearsDownTheControllerStreamBeforeEitherSyncpointRelease)
{
    SwitchHost1xDisplayStream stream;
    ASSERT_EQ(tegra_host1x_syncpt_alloc(&gFailedProducer), 0);
    ASSERT_EQ(switch_host1x_display_start(&stream), 0);
    const switch_gpu_backing_token token = {1, 1, SWITCH_GPU_ENGINE_VIC, gFailedProducer};
    const uint32_t word = 0xdc0, address = 0x20000000;
    DisplayCompletion done;
    ASSERT_EQ(
        switch_host1x_display_submit(&stream, &token, 1, &word, &address, 1, 0x500, 5, DisplayCompletion::heard, &done),
        0);
    DisplayExecution execution;
    execution.run();
    ASSERT_TRUE(execution.controller.empty());
    gCpuIncrements = true;
    gRegisterWrites.clear();
    ASSERT_EQ(tegra_host1x_syncpt_release_failed(gFailedProducer, 1, 1), 0);
    execution.run();
    EXPECT_TRUE(execution.controller.empty());
    EXPECT_EQ(gProducerPhysical, -5);
    ASSERT_FALSE(gRegisterWrites.empty());
    auto teardown = gRegisterWrites.end(), release = gRegisterWrites.end();
    for (auto it = gRegisterWrites.begin(); it != gRegisterWrites.end(); ++it) {
        if (it->first == kSyncOffset + SYNC_CH_TEARDOWN)
            teardown = it;
        if (it->first >= kSyncOffset + 0xf60 && it->first < kSyncOffset + 0xf78 && release == gRegisterWrites.end())
            release = it;
    }
    ASSERT_NE(teardown, gRegisterWrites.end());
    ASSERT_NE(release, gRegisterWrites.end());
    EXPECT_LT(teardown, release);
    EXPECT_EQ(syncpoint(stream.syncpt), stream.fence.threshold);
    EXPECT_EQ(done.calls, 0u);
    stream.fence.signaled(&stream.fence, 0);
    EXPECT_EQ(done.calls, 1u);
    EXPECT_EQ(done.status, SwitchHost1xDisplayStream::kCancelled);
    EXPECT_EQ(
        switch_host1x_display_submit(&stream, &token, 1, &word, &address, 1, 0x500, 5, DisplayCompletion::heard, &done),
        -5);
    EXPECT_EQ(gDisplayStream, nullptr);
}

namespace {
void alternatingFailures(bool vicFirst)
{
    SwitchHost1xDisplayStream stream;
    ASSERT_EQ(tegra_host1x_syncpt_alloc(&gFailedProducer), 0);
    uint32_t source = gFailedProducer;
    gFailedNodes.clear();
    gFailurePasses = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        uint32_t syncpt = 0;
        ASSERT_EQ(tegra_host1x_syncpt_alloc(&syncpt), 0);
        const uint32_t engine = (bool(i & 1) == vicFirst) ? SWITCH_GPU_ENGINE_NVDEC : SWITCH_GPU_ENGINE_VIC;
        gFailedNodes.push_back({engine, syncpt, source, 0});
        source = syncpt;
    }
    ASSERT_EQ(switch_host1x_display_start(&stream), 0);
    const switch_gpu_backing_token token = {1, 1, gFailedNodes.back().engine, source};
    const uint32_t word = 0xdc0, address = 0x20000000;
    DisplayCompletion done;
    ASSERT_EQ(
        switch_host1x_display_submit(&stream, &token, 1, &word, &address, 1, 0x500, 5, DisplayCompletion::heard, &done),
        0);
    DisplayExecution execution;
    execution.run();
    ASSERT_TRUE(execution.controller.empty());
    gCpuIncrements = true;
    ASSERT_EQ(tegra_host1x_syncpt_release_failed(gFailedProducer, 1, 1), 0);
    EXPECT_GE(gFailurePasses, 3u);
    for (const auto &node : gFailedNodes) {
        EXPECT_EQ(node.physical, -5);
        syncpoint(node.syncpt) = 1;
    }
    execution.run();
    EXPECT_TRUE(execution.controller.empty());
    EXPECT_EQ(done.calls, 0u);
    stream.fence.signaled(&stream.fence, 0);
    EXPECT_EQ(done.calls, 1u);
    EXPECT_EQ(done.status, SwitchHost1xDisplayStream::kCancelled);
    EXPECT_EQ(gDisplayStream, nullptr);
}
}  // namespace

TEST_F(Host1xSubmit, NvdecThenVicFailuresReachTheDisplayBeforeTheAbandonedBridgeCanRun)
{
    alternatingFailures(false);
}

TEST_F(Host1xSubmit, VicThenNvdecFailuresReachTheDisplayBeforeTheAbandonedBridgeCanRun)
{
    alternatingFailures(true);
}

TEST_F(Host1xSubmit, AnUnrelatedFailedProducerCannotCancelTheControllerStream)
{
    SwitchHost1xDisplayStream stream;
    uint32_t producer = 0;
    ASSERT_EQ(tegra_host1x_syncpt_alloc(&producer), 0);
    ASSERT_EQ(tegra_host1x_syncpt_alloc(&gFailedProducer), 0);
    ASSERT_EQ(switch_host1x_display_start(&stream), 0);
    const switch_gpu_backing_token token = {1, 1, SWITCH_GPU_ENGINE_VIC, producer};
    const uint32_t word = 0xdc0, address = 0x20000000;
    DisplayCompletion done;
    ASSERT_EQ(
        switch_host1x_display_submit(&stream, &token, 1, &word, &address, 1, 0x500, 5, DisplayCompletion::heard, &done),
        0);
    DisplayExecution execution;
    execution.run();
    ASSERT_TRUE(execution.controller.empty());
    gCpuIncrements = true;
    ASSERT_EQ(tegra_host1x_syncpt_release_failed(gFailedProducer, 1, 1), 0);
    EXPECT_EQ(gDisplayTeardowns, 0u);
    EXPECT_EQ(stream.status, 0);
    EXPECT_EQ(gDisplayStream, &stream);
    EXPECT_EQ(done.calls, 0u);
    syncpoint(producer) = 1;
    execution.run();
    ASSERT_FALSE(execution.controller.empty());
    EXPECT_EQ(execution.controller.front(), std::make_pair(word, address));
    stream.fence.signaled(&stream.fence, 0);
    EXPECT_EQ(done.calls, 1u);
    EXPECT_EQ(done.status, 0);
}

TEST_F(Host1xSubmit, AWithdrawnStreamOwnsItsFenceUntilTheCallbackDrains)
{
    SwitchHost1xDisplayStream stream;
    uint32_t producer = 0;
    ASSERT_EQ(tegra_host1x_syncpt_alloc(&producer), 0);
    ASSERT_EQ(switch_host1x_display_start(&stream), 0);
    const switch_gpu_backing_token token = {1, 1, SWITCH_GPU_ENGINE_NVDEC, producer};
    const uint32_t word = 0xdc0, address = 0x20000000;
    DisplayCompletion done;
    ASSERT_EQ(
        switch_host1x_display_submit(&stream, &token, 1, &word, &address, 1, 0x500, 5, DisplayCompletion::heard, &done),
        0);
    gCpuIncrements = true;
    EXPECT_EQ(switch_host1x_display_cancel(&stream), SwitchHost1xDisplayStream::kCancelled);
    EXPECT_EQ(
        switch_host1x_display_submit(&stream, &token, 1, &word, &address, 1, 0x500, 5, DisplayCompletion::heard, &done),
        TEGRA_HOST1X_EAGAIN);
    stream.fence.signaled(&stream.fence, 0);
    EXPECT_EQ(done.calls, 1u);
    EXPECT_EQ(done.status, SwitchHost1xDisplayStream::kCancelled);
    ASSERT_EQ(
        switch_host1x_display_submit(&stream, &token, 1, &word, &address, 1, 0x500, 5, DisplayCompletion::heard, &done),
        0);
    syncpoint(stream.syncpt) = stream.fence.threshold;
    stream.fence.signaled(&stream.fence, 0);
    EXPECT_EQ(done.calls, 2u);
    EXPECT_EQ(done.status, 0);
}

// A filter waiter on another CPU that sees a released syncpoint move reads the failure stored before it, so every CPU
// increment of a release, the display stream's and the producer's, orders those stores first.
TEST_F(Host1xSubmit, EveryCpuReleaseIncrementIsAPublishingWrite)
{
    SwitchHost1xDisplayStream stream;
    ASSERT_EQ(tegra_host1x_syncpt_alloc(&gFailedProducer), 0);
    ASSERT_EQ(switch_host1x_display_start(&stream), 0);
    const uint32_t from = syncpoint(gFailedProducer);
    const switch_gpu_backing_token token = {from + 1, 1, SWITCH_GPU_ENGINE_VIC, gFailedProducer};
    const uint32_t word = 0xdc0, address = 0x20000000;
    DisplayCompletion done;
    ASSERT_EQ(
        switch_host1x_display_submit(&stream, &token, 1, &word, &address, 1, 0x500, 5, DisplayCompletion::heard, &done),
        0);
    gCpuIncrements = true;
    ASSERT_EQ(tegra_host1x_syncpt_release_failed(gFailedProducer, from + 2, 2), 0);
    EXPECT_EQ(syncpoint(stream.syncpt), stream.fence.threshold);
    EXPECT_EQ(syncpoint(gFailedProducer), from + 2);
    EXPECT_TRUE(gPlainIncrements.empty());
    stream.fence.signaled(&stream.fence, 0);
    EXPECT_EQ(done.status, SwitchHost1xDisplayStream::kCancelled);
}

// A CPU increment that does not take is a hardware fact, whichever syncpoint it was: one line naming it, never a
// cancelled display stream that leaves the failed producer's waiters stuck behind an error code.
TEST_F(Host1xSubmit, ACpuIncrementThatDoesNotTakePanics)
{
    SwitchHost1xDisplayStream stream;
    ASSERT_EQ(tegra_host1x_syncpt_alloc(&gFailedProducer), 0);
    ASSERT_EQ(switch_host1x_display_start(&stream), 0);
    const uint32_t from = syncpoint(gFailedProducer);
    const switch_gpu_backing_token token = {from + 1, 1, SWITCH_GPU_ENGINE_NVDEC, gFailedProducer};
    const uint32_t word = 0xdc0, address = 0x20000000;
    DisplayCompletion done;
    ASSERT_EQ(
        switch_host1x_display_submit(&stream, &token, 1, &word, &address, 1, 0x500, 5, DisplayCompletion::heard, &done),
        0);
    gCpuIncrements = true;
    gStuckSyncpt = stream.syncpt;
    EXPECT_THROW(tegra_host1x_syncpt_release_failed(gFailedProducer, from + 1, 1), std::runtime_error);
    IOLockUnlock(gSubmitLock);
    stream.fence.signaled(&stream.fence, 0);
    EXPECT_EQ(done.status, SwitchHost1xDisplayStream::kCancelled);

    gStuckSyncpt = gFailedProducer;
    EXPECT_THROW(tegra_host1x_syncpt_release_failed(gFailedProducer, from + 1, 1), std::runtime_error);
    IOLockUnlock(gSubmitLock);
}

// The cancelled jobs' failure is propagated once, from the value the stopped engine reached, however many increments
// the CPU then makes.
TEST_F(Host1xSubmit, ReleaseThroughPropagatesTheFailureOnceForAllItsIncrements)
{
    ASSERT_EQ(tegra_host1x_syncpt_alloc(&gFailedProducer), 0);
    gProducerPhysical = -5;
    gCpuIncrements = true;
    const uint32_t from = syncpoint(gFailedProducer);
    ASSERT_EQ(tegra_host1x_syncpt_release_through(gFailedProducer, from + 5), 0);
    EXPECT_EQ(syncpoint(gFailedProducer), from + 5);
    EXPECT_EQ(gFailurePasses, 1u);
    EXPECT_TRUE(gPlainIncrements.empty());
}

TEST_F(Host1xSubmit, AReleaseOfASyncpointNotHandedOutOrFartherThanAskedIsRefusedBeforeAnyScan)
{
    uint32_t id = 0;
    ASSERT_EQ(tegra_host1x_syncpt_alloc(&id), 0);
    gCpuIncrements = true;
    const uint32_t from = syncpoint(id);
    EXPECT_EQ(tegra_host1x_syncpt_release_failed(9, 2, 2), TEGRA_HOST1X_EINVAL) << "the boot chain's";
    EXPECT_EQ(tegra_host1x_syncpt_release_failed(id + 1, 2, 2), TEGRA_HOST1X_EINVAL) << "not handed out yet";
    EXPECT_EQ(tegra_host1x_syncpt_release_failed(id, from + 3, 2), TEGRA_HOST1X_EINVAL);
    EXPECT_EQ(tegra_host1x_syncpt_release_failed(id, from, 2), TEGRA_HOST1X_OK);
    EXPECT_EQ(gFailurePasses, 0u);
    EXPECT_EQ(syncpoint(id), from);
    EXPECT_EQ(gDisplayTeardowns, 0u);
}

}  // namespace
