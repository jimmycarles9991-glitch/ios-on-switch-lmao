// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "switch_host1x_submit_harness.h"

namespace {

TEST_F(Host1xSubmit, BringUpMakesTheEnginesAndTheDisplaysChannelsAlone)
{
    EXPECT_EQ(gAllocated, 2u * (TEGRA_HOST1X_CHANNEL_DISPLAY + 1));
}

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
    for (uint32_t channel = 0; channel <= TEGRA_HOST1X_CHANNEL_DISPLAY; ++channel)
        switch_mmio_write32(gHostRegisters, channel * kChannelStride + CH_CHANNELCTRL, 0x5);
    IOLockLock(gSubmitLock);
    switch_host1x_channels_program(163200);
    IOLockUnlock(gSubmitLock);
    for (uint32_t channel = 0; channel <= TEGRA_HOST1X_CHANNEL_DISPLAY; ++channel)
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
        EXPECT_FALSE(gChannels[channel].lock->held);
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
        {"../../TegraNVJPG/TegraNVJPGFlight.cpp", "int tegra_nvjpg_start(",
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
          "../../TegraVIC/TegraVIC.cpp", "../../TegraNVENC/TegraNVENC.cpp", "../../TegraNVJPG/TegraNVJPG.cpp",
          "../../TegraNVJPG/TegraNVJPGFlight.cpp"})
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

// The bridged tests share one stream, as the display does, and two syncpoints: the suite's pool has 32, never freed.
struct Bridged {
    SwitchHost1xDisplayStream stream;
    uint32_t bridgeSyncpt = 0, unrelated = 0;

    static Bridged &get()
    {
        static Bridged shared;
        if (!shared.bridgeSyncpt) {
            EXPECT_EQ(switch_host1x_display_start(&shared.stream), 0);
            EXPECT_EQ(tegra_host1x_syncpt_alloc(&shared.bridgeSyncpt), 0);
            EXPECT_EQ(tegra_host1x_syncpt_alloc(&shared.unrelated), 0);
        }
        return shared;
    }
};

// A GPU-rendered window B set: the stream WAITs on the bridge's syncpoint before anything else, writes once it is
// reached, and takes the producers' outcome from the bridge, which it lets go outside the channel lock.
TEST_F(Host1xSubmit, ABridgedStreamWaitsOnTheBridgeAndTakesItsOutcome)
{
    auto &stream = Bridged::get().stream;
    const uint32_t bridgeSyncpt = Bridged::get().bridgeSyncpt;
    for (const int outcome : {1, -5}) {
        SCOPED_TRACE(outcome);
        switch_gpu_bridge bridge = {bridgeSyncpt, syncpoint(bridgeSyncpt) + 2, 0, 0, false};
        const uint32_t word = 0xdc0, address = 0x20000000;
        DisplayCompletion done;
        ASSERT_EQ(switch_host1x_display_submit(&stream, nullptr, 0, &word, &address, 1, 0x500, 5,
                                               DisplayCompletion::heard, &done, &bridge),
                  0);
        DisplayExecution execution;
        execution.run();
        EXPECT_TRUE(execution.controller.empty());
        syncpoint(bridge.syncpoint) = bridge.threshold;
        bridge.status = outcome;
        execution.run();
        EXPECT_EQ(execution.controller,
                  (std::vector<std::pair<uint32_t, uint32_t>>{{word, address}, {0x041, 0x500}, {0x041, 5}}));
        EXPECT_EQ(bridge.releases, 0u);
        stream.fence.signaled(&stream.fence, 0);
        EXPECT_EQ(done.calls, 1u);
        EXPECT_EQ(done.status, outcome < 0 ? outcome : 0);
        EXPECT_EQ(bridge.releases, 1u);
        EXPECT_FALSE(bridge.lockedWhenAsked);
        EXPECT_EQ(stream.bridge, nullptr);
    }
}

// A GPU producer that fails releases the bridge's syncpoint as failed: the stream waiting on it is torn down before
// that release can let it write, as for a failed VIC or NVDEC producer.
TEST_F(Host1xSubmit, AFailedBridgeTearsDownItsStreamBeforeItsSyncpointRelease)
{
    auto &stream = Bridged::get().stream;
    const uint32_t unrelated = Bridged::get().unrelated;
    switch_gpu_bridge bridge = {Bridged::get().bridgeSyncpt, syncpoint(Bridged::get().bridgeSyncpt) + 2, 0, 0, false};
    const uint32_t word = 0xdc0, address = 0x20000000;
    DisplayCompletion done;
    ASSERT_EQ(switch_host1x_display_submit(&stream, nullptr, 0, &word, &address, 1, 0x500, 5, DisplayCompletion::heard,
                                           &done, &bridge),
              0);
    DisplayExecution execution;
    execution.run();
    gCpuIncrements = true;
    ASSERT_EQ(tegra_host1x_syncpt_release_failed(unrelated, syncpoint(unrelated) + 1, 1), 0);
    EXPECT_EQ(gDisplayTeardowns, 0u) << "a syncpoint the stream does not wait on";
    gRegisterWrites.clear();
    bridge.status = -5;
    ASSERT_EQ(tegra_host1x_syncpt_release_failed(bridge.syncpoint, bridge.threshold, 2), 0);
    execution.run();
    EXPECT_TRUE(execution.controller.empty());
    auto teardown = gRegisterWrites.end(), release = gRegisterWrites.end();
    for (auto it = gRegisterWrites.begin(); it != gRegisterWrites.end(); ++it) {
        if (it->first == kSyncOffset + SYNC_CH_TEARDOWN && teardown == gRegisterWrites.end())
            teardown = it;
        if (it->first >= kSyncOffset + 0xf60 && it->first < kSyncOffset + 0xf78 && release == gRegisterWrites.end())
            release = it;
    }
    ASSERT_NE(teardown, gRegisterWrites.end());
    ASSERT_NE(release, gRegisterWrites.end());
    EXPECT_LT(teardown, release);
    bridge.why = SWITCH_GPU_BACKING_WRITER_GONE;
    stream.fence.signaled(&stream.fence, 0);
    EXPECT_EQ(done.calls, 1u);
    EXPECT_EQ(done.status, SWITCH_GPU_BACKING_WRITER_GONE) << "the cancelled set's completion says why";
    EXPECT_EQ(stream.bridgeWhy, SWITCH_GPU_BACKING_WRITER_GONE);
    EXPECT_FALSE(bridge.whyReadAfterRelease);
    EXPECT_EQ(bridge.releases, 1u);
}

// A failure handled between the stream's syncpoint read and its publication: the release raised the syncpoint with no
// stream to cancel, so a stream queued now would pass its WAIT over unfinished writes. Read under the channel lock in
// the hold that publishes the stream, the failure refuses it.
TEST_F(Host1xSubmit, AFailureReleasedJustBeforeThePublicationRefusesTheStream)
{
    auto &stream = Bridged::get().stream;
    switch_gpu_bridge bridge = {Bridged::get().bridgeSyncpt, syncpoint(Bridged::get().bridgeSyncpt) + 2, 0, 0, false};
    bridge.beforeLock = [](switch_gpu_bridge *b) {
        b->failed = b->why = SWITCH_GPU_BACKING_WRITER_GONE;
        gCpuIncrements = true;
        EXPECT_EQ(tegra_host1x_syncpt_release_failed(b->syncpoint, b->threshold, 2), 0);
    };
    const uint32_t word = 0xdc0, address = 0x20000000, put = doorbell(TEGRA_HOST1X_CHANNEL_DISPLAY);
    DisplayCompletion done;
    EXPECT_EQ(switch_host1x_display_submit(&stream, nullptr, 0, &word, &address, 1, 0x500, 5, DisplayCompletion::heard,
                                           &done, &bridge),
              SWITCH_GPU_BACKING_WRITER_GONE);
    EXPECT_EQ(syncpoint(bridge.syncpoint), bridge.threshold) << "the release ran inside the window";
    EXPECT_EQ(doorbell(TEGRA_HOST1X_CHANNEL_DISPLAY), put) << "no stream gathered after its producer failed";
    EXPECT_EQ(gDisplayStream, nullptr);
    EXPECT_EQ(stream.bridge, nullptr);
    EXPECT_EQ(bridge.releases, 0u) << "a refused bridge stays its caller's";
    EXPECT_FALSE(bridge.failedReadUnlocked);
    EXPECT_EQ(done.calls, 0u);
}

// A producer failed and the bridge has not stored it yet (its retire pass is still to come): the producers' own status,
// read in the publishing hold, refuses the stream.
TEST_F(Host1xSubmit, AProducerFailureTheBridgeHasNotStoredYetRefusesTheStream)
{
    auto &stream = Bridged::get().stream;
    switch_gpu_bridge bridge = {Bridged::get().bridgeSyncpt, syncpoint(Bridged::get().bridgeSyncpt) + 2, 0, 0, false};
    bridge.beforeLock = [](switch_gpu_bridge *b) {
        b->sourceFailure = SWITCH_GPU_BACKING_WRITER_GONE;
    };
    const uint32_t word = 0xdc0, address = 0x20000000, put = doorbell(TEGRA_HOST1X_CHANNEL_DISPLAY);
    DisplayCompletion done;
    EXPECT_EQ(switch_host1x_display_submit(&stream, nullptr, 0, &word, &address, 1, 0x500, 5, DisplayCompletion::heard,
                                           &done, &bridge),
              SWITCH_GPU_BACKING_WRITER_GONE);
    EXPECT_TRUE(bridge.sourceReadLocked);
    EXPECT_EQ(doorbell(TEGRA_HOST1X_CHANNEL_DISPLAY), put);
    EXPECT_EQ(gDisplayStream, nullptr);
    EXPECT_EQ(bridge.releases, 0u);
}

// A failure dated after the writes reached DRAM fails a stream that already wrote its set: its completion carries the
// bridge's reason, read before the bridge goes.
TEST_F(Host1xSubmit, AWrittenSetsLateFailureCarriesItsReason)
{
    auto &stream = Bridged::get().stream;
    switch_gpu_bridge bridge = {Bridged::get().bridgeSyncpt, syncpoint(Bridged::get().bridgeSyncpt) + 2, 0, 0, false};
    const uint32_t word = 0xdc0, address = 0x20000000;
    DisplayCompletion done;
    ASSERT_EQ(switch_host1x_display_submit(&stream, nullptr, 0, &word, &address, 1, 0x500, 5, DisplayCompletion::heard,
                                           &done, &bridge),
              0);
    EXPECT_EQ(bridge.statusReads, 0u) << "the publishing hold's reads decide; no queue.lock trip a frame before it";
    syncpoint(bridge.syncpoint) = bridge.threshold;
    DisplayExecution execution;
    execution.run();
    EXPECT_FALSE(execution.controller.empty());
    bridge.status = -5;
    bridge.why = SWITCH_GPU_BACKING_WRITE_REFUSED;
    stream.fence.signaled(&stream.fence, 0);
    EXPECT_EQ(done.calls, 1u);
    EXPECT_EQ(done.status, SWITCH_GPU_BACKING_WRITE_REFUSED);
    EXPECT_EQ(stream.status, 0) << "the stream itself ran";
    EXPECT_EQ(stream.bridgeWhy, SWITCH_GPU_BACKING_WRITE_REFUSED);
    EXPECT_FALSE(bridge.whyReadAfterRelease);
}

// A mixed set's stream past its bridge's threshold (the GPU drained its slot's covering job), still waiting on VIC:
// another slot's release on the same producer syncpoint, to a later threshold, is not this stream's. It writes once
// VIC is done.
TEST_F(Host1xSubmit, AnotherSlotsReleaseOnTheProducerSyncpointLeavesADrainedStream)
{
    auto &shared = Bridged::get();
    auto &stream = shared.stream;
    const uint32_t vicAt = syncpoint(shared.unrelated) + 1;
    const switch_gpu_backing_token vic = {vicAt, 1, SWITCH_GPU_ENGINE_VIC, shared.unrelated};
    switch_gpu_bridge bridge = {shared.bridgeSyncpt, syncpoint(shared.bridgeSyncpt) + 1, 0, 0, false};
    const uint32_t word = 0xdc0, address = 0x20000000;
    DisplayCompletion done;
    ASSERT_EQ(switch_host1x_display_submit(&stream, &vic, 1, &word, &address, 1, 0x500, 5, DisplayCompletion::heard,
                                           &done, &bridge),
              0);
    syncpoint(bridge.syncpoint) = bridge.threshold;
    DisplayExecution execution;
    execution.run();
    ASSERT_TRUE(execution.controller.empty()) << "waiting on VIC";
    gCpuIncrements = true;
    ASSERT_EQ(tegra_host1x_syncpt_release_failed(bridge.syncpoint, bridge.threshold + 2, 2), 0);
    EXPECT_EQ(gDisplayTeardowns, 0u);
    EXPECT_EQ(stream.status, 0);
    EXPECT_EQ(gDisplayStream, &stream);
    syncpoint(shared.unrelated) = vicAt;
    execution.run();
    EXPECT_FALSE(execution.controller.empty());
    stream.fence.signaled(&stream.fence, 0);
    EXPECT_EQ(done.calls, 1u);
    EXPECT_EQ(done.status, 0);
    EXPECT_EQ(bridge.releases, 1u);
}

// host1x stopping the display channel on an illegal opcode under a stream still out is the kernel's own failure: the
// accepted swap cannot present. With none out (none queued, or it completed) the channel's other jobs fail as usual.
TEST_F(Host1xSubmit, AStopOfTheDisplayChannelUnderAStreamStillOutPanics)
{
    auto &stream = Bridged::get().stream;
    IOLockLock(gSubmitLock);
    EXPECT_NO_THROW(switch_host1x_display_channel_stopped_locked()) << "no stream out";
    IOLockUnlock(gSubmitLock);
    switch_gpu_bridge bridge = {Bridged::get().bridgeSyncpt, syncpoint(Bridged::get().bridgeSyncpt) + 1, 0, 0, false};
    const uint32_t word = 0xdc0, address = 0x20000000;
    DisplayCompletion done;
    ASSERT_EQ(switch_host1x_display_submit(&stream, nullptr, 0, &word, &address, 1, 0x500, 5, DisplayCompletion::heard,
                                           &done, &bridge),
              0);
    IOLockLock(gSubmitLock);
    EXPECT_THROW(switch_host1x_display_channel_stopped_locked(), std::runtime_error);
    IOLockUnlock(gSubmitLock);
    syncpoint(bridge.syncpoint) = bridge.threshold;
    DisplayExecution execution;
    execution.run();
    IOLockLock(gSubmitLock);
    EXPECT_NO_THROW(switch_host1x_display_channel_stopped_locked()) << "the stream ran to its syncpoint";
    IOLockUnlock(gSubmitLock);
    stream.fence.signaled(&stream.fence, 0);
    EXPECT_EQ(done.calls, 1u);
}

// A bridge whose producers already failed is refused, and stays its caller's.
TEST_F(Host1xSubmit, AnAlreadyFailedBridgeIsRefusedAndStaysTheCallers)
{
    auto &stream = Bridged::get().stream;
    switch_gpu_bridge bridge = {0, 2, -5, 0, false};
    bridge.failed = -5;
    const uint32_t word = 0xdc0, address = 0x20000000;
    DisplayCompletion done;
    EXPECT_EQ(switch_host1x_display_submit(&stream, nullptr, 0, &word, &address, 1, 0x500, 5, DisplayCompletion::heard,
                                           &done, &bridge),
              -5);
    EXPECT_EQ(bridge.releases, 0u);
    EXPECT_EQ(stream.bridge, nullptr);
    EXPECT_EQ(gDisplayStream, nullptr);
    EXPECT_FALSE(bridge.lockedWhenAsked);
    EXPECT_EQ(switch_host1x_display_submit(&stream, nullptr, 0, &word, &address, 1, 0x500, 5, DisplayCompletion::heard,
                                           &done),
              TEGRA_HOST1X_EINVAL)
        << "a stream needs a producer to wait on";
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

TEST_F(Host1xSubmit, AReleaseClosesTheGpuBridgesGateAfterTransitiveCancelAndBeforeItsFirstIncrement)
{
    ASSERT_EQ(tegra_host1x_syncpt_alloc(&gFailedProducer), 0);
    const uint32_t from = syncpoint(gFailedProducer);
    gCpuIncrements = true;
    int physicalAtClose = 0;
    gOnGateClose = [&] {
        physicalAtClose = gProducerPhysical;
    };
    ASSERT_EQ(tegra_host1x_syncpt_release_failed(gFailedProducer, from + 2, 2), 0);
    EXPECT_EQ(physicalAtClose, -5) << "transitive cancellations ran before closing the gate";
    EXPECT_EQ(gGateCloses, std::vector<uint32_t>{from});
    EXPECT_EQ(syncpoint(gFailedProducer), from + 2);
}

TEST_F(Host1xSubmit, CancellingWaitersCancelsTheDisplayStreamAndMakesNoIncrement)
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
    ASSERT_EQ(tegra_host1x_syncpt_cancel_waiters(gFailedProducer, from + 2), 0);
    EXPECT_EQ(syncpoint(gFailedProducer), from) << "the producer makes its own increments";
    EXPECT_TRUE(gGateCloses.empty());
    stream.fence.signaled(&stream.fence, 0);
    EXPECT_EQ(done.status, SwitchHost1xDisplayStream::kCancelled);
    EXPECT_EQ(tegra_host1x_syncpt_cancel_waiters(gFailedProducer, from), TEGRA_HOST1X_OK) << "nothing short of it";
    EXPECT_EQ(tegra_host1x_syncpt_cancel_waiters(9, 2), TEGRA_HOST1X_EINVAL) << "the boot chain's";
}

TEST_F(Host1xSubmit, CancellingWaitersCancelsABridgedDisplayStreamWithoutIncrementingTheBridgeSyncpt)
{
    auto &shared = Bridged::get();
    auto &stream = shared.stream;
    const uint32_t from = syncpoint(shared.bridgeSyncpt);
    switch_gpu_bridge bridge = {shared.bridgeSyncpt, from + 2, -5, 0, false};
    bridge.why = SWITCH_GPU_BACKING_WRITER_GONE;
    const uint32_t word = 0xdc0, address = 0x20000000;
    DisplayCompletion done;
    ASSERT_EQ(switch_host1x_display_submit(&stream, nullptr, 0, &word, &address, 1, 0x500, 5, DisplayCompletion::heard,
                                           &done, &bridge),
              0);
    gCpuIncrements = true;
    __atomic_store_n(&bridge.failed, -5, __ATOMIC_RELEASE);
    ASSERT_EQ(tegra_host1x_syncpt_cancel_waiters(bridge.syncpoint, bridge.threshold), 0);
    EXPECT_EQ(syncpoint(bridge.syncpoint), from);
    EXPECT_TRUE(gGateCloses.empty());
    EXPECT_EQ(stream.status, SwitchHost1xDisplayStream::kCancelled);
    stream.fence.signaled(&stream.fence, 0);
    EXPECT_EQ(done.calls, 1u);
    EXPECT_EQ(done.status, SWITCH_GPU_BACKING_WRITER_GONE);
    EXPECT_EQ(bridge.releases, 1u);
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
    IOLockUnlock(gChannels[TEGRA_HOST1X_CHANNEL_DISPLAY].lock);
    IOLockUnlock(gSubmitLock);
    stream.fence.signaled(&stream.fence, 0);
    EXPECT_EQ(done.status, SwitchHost1xDisplayStream::kCancelled);

    gStuckSyncpt = gFailedProducer;
    EXPECT_THROW(tegra_host1x_syncpt_release_failed(gFailedProducer, from + 1, 1), std::runtime_error);
    IOLockUnlock(gChannels[TEGRA_HOST1X_CHANNEL_DISPLAY].lock);
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

#if SWITCH_HOST1X_STATS
uint32_t &channelRegister(uint32_t channel, uint32_t reg)
{
    return *reinterpret_cast<uint32_t *>(gHostRegisters + channel * kChannelStride + reg);
}

TEST_F(Host1xSubmit, ADebugProgramStartsEveryChannelsTickAndStallCountersFromZero)
{
    for (uint32_t channel = 0; channel <= TEGRA_HOST1X_CHANNEL_DISPLAY; ++channel) {
        channelRegister(channel, CH_CHANNELCTRL) = 0x4;
        for (const uint32_t reg : {CH_TICKCOUNT_HI, CH_TICKCOUNT_LO, CH_STALLCOUNT_HI, CH_STALLCOUNT_LO})
            channelRegister(channel, reg) = 0x1234;
        channelRegister(channel, CH_STALLCTRL) = 0;
    }
    IOLockLock(gSubmitLock);
    switch_host1x_channels_program(163200);
    IOLockUnlock(gSubmitLock);
    for (uint32_t channel = 0; channel <= TEGRA_HOST1X_CHANNEL_DISPLAY; ++channel) {
        EXPECT_EQ(channelRegister(channel, CH_CHANNELCTRL), 0x1u) << channel;
        EXPECT_EQ(channelRegister(channel, CH_STALLCTRL), 0x1u) << channel;
        for (const uint32_t reg : {CH_TICKCOUNT_HI, CH_TICKCOUNT_LO, CH_STALLCOUNT_HI, CH_STALLCOUNT_LO})
            EXPECT_EQ(channelRegister(channel, reg), 0u) << channel << " " << reg;
    }
}

TEST_F(Host1xSubmit, TheStatsLineReadsTheChannelCountersOnlyWhileHost1xRuns)
{
    constexpr uint32_t vic = TEGRA_HOST1X_CHANNEL_VIC;
    gGatherStats = {};
    IOLockLock(gSubmitLock);
    switch_host1x_channels_program(163200);
    IOLockUnlock(gSubmitLock);
    switch_test_log.clear();
    switch_test_now_abs = 1000000000;
    job(vic, 1, 1);
    EXPECT_EQ(switch_test_log, "");
    channelRegister(vic, CH_TICKCOUNT_HI) = 1;
    channelRegister(vic, CH_TICKCOUNT_LO) = 0x2930b80;
    channelRegister(vic, CH_STALLCOUNT_LO) = 163200000;
    switch_test_now_abs = 6000000000;
    job(vic, 1, 2);
    EXPECT_THAT(switch_test_log, testing::EndsWith(" clocked=0/26581853/0/0/0us stalled=0/1000000/0/0/0us"));
    uint32_t *words = nullptr;
    ASSERT_EQ(switch_host1x_job_reserve(vic, 2, &words), TEGRA_HOST1X_OK);
    IOLockLock(gSubmitLock);
    switch_host1x_channels_stop();
    IOLockUnlock(gSubmitLock);
    switch_test_now_abs = 12000000000;
    EXPECT_EQ(switch_host1x_job_submit(vic, words, 2, 170, 3), TEGRA_HOST1X_ENODEV);
    EXPECT_THAT(switch_test_log, testing::EndsWith(" counters=off"));
    switch_test_now_abs = 0;
    gGatherStats = {};
}
#endif

}  // namespace

// A channel stopped on an illegal opcode (SwitchHost1xErrors.cpp): its jobs' bounded fences fail at once, through
// each queue's own cancellation; the display's stream is cancelled as its own cancel does.
TEST_F(Host1xSubmit, AStoppedEnginesChannelFailsItsJobsSyncpointOnceAndNoOtherChannels)
{
    constexpr uint32_t channel = TEGRA_HOST1X_CHANNEL_NVENC;
    syncpoint(171) = 10;
    uint32_t *words = nullptr;
    for (const uint32_t threshold : {11u, 12u}) {
        ASSERT_EQ(switch_host1x_job_reserve(channel, 4, &words), TEGRA_HOST1X_OK);
        words[0] = tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_NVENC, 0, 0);
        ASSERT_EQ(switch_host1x_job_submit(channel, words, 4, 171, threshold), TEGRA_HOST1X_OK);
    }
    gFencesFailed.clear();
    char line[64];
    switch_host1x_channels_failed(channel, line, sizeof(line));
    EXPECT_EQ(gFencesFailed, (std::vector<std::pair<uint32_t, int>>{{171, TEGRA_HOST1X_ETIMEDOUT}}));
    EXPECT_STREQ(line, "syncpoint 171: 1 failed");
    gFencesFailed.clear();
    switch_host1x_channels_failed(TEGRA_HOST1X_CHANNEL_NVJPG, line, sizeof(line));
    EXPECT_TRUE(gFencesFailed.empty());
    EXPECT_STREQ(line, "");
    gRearmed.clear();
    gFencesResumed.clear();
    tegra_host1x_channel_teardown(channel);
    tegra_host1x_channel_restart(channel);
    EXPECT_EQ(gRearmed, std::vector<uint32_t>{channel}) << "a restart unmasks the channel's illegal opcode again";
    EXPECT_EQ(gFencesResumed, std::vector<uint32_t>{171}) << "and its syncpoint is no longer stopped";
}

// An error on the display's channel asks the display first, in the same hold of the submit lock and before any fence
// fails: with an accepted swap still out the display panics there (the harness's panic throws), so no fence has failed
// and the lock is still held. Another channel's error never asks it. With the swap no longer out the display returns
// and the channel's fenced jobs fail as usual; nothing cancels the stream on host1x's side.
TEST_F(Host1xSubmit, AStoppedDisplayChannelAsksTheDisplayBeforeAnyFenceFails)
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
    gFencesFailed.clear();
    char line[64];
    EXPECT_THROW(switch_host1x_channels_failed(TEGRA_HOST1X_CHANNEL_DISPLAY, line, sizeof(line)), std::runtime_error)
        << "an accepted swap out: the display panics";
    EXPECT_TRUE(gFencesFailed.empty()) << "before any fence failed";
    ASSERT_TRUE(gChannels[TEGRA_HOST1X_CHANNEL_DISPLAY].lock->held) << "inside the display channel lock";
    IOLockUnlock(gChannels[TEGRA_HOST1X_CHANNEL_DISPLAY].lock);
    EXPECT_NE(stream.status, SwitchHost1xDisplayStream::kCancelled) << "host1x no longer cancels the stream";

    EXPECT_NO_THROW(switch_host1x_channels_failed(TEGRA_HOST1X_CHANNEL_VIC, line, sizeof(line)))
        << "another channel's error does not ask the display";
    EXPECT_FALSE(gChannels[TEGRA_HOST1X_CHANNEL_VIC].lock->held);

    syncpoint(stream.syncpt) = stream.fence.threshold;
    gFencesFailed.clear();
    EXPECT_NO_THROW(switch_host1x_channels_failed(TEGRA_HOST1X_CHANNEL_DISPLAY, line, sizeof(line)))
        << "the stream ran to its syncpoint: nothing out";
    EXPECT_FALSE(gFencesFailed.empty()) << "then the channel's fenced jobs fail as usual";
    EXPECT_FALSE(gChannels[TEGRA_HOST1X_CHANNEL_DISPLAY].lock->held) << "one hold, released at the end";
    EXPECT_EQ(done.calls, 0u);
}

TEST_F(Host1xSubmit, DisplayAndEngineChannelsUseIndependentLocks)
{
    EXPECT_NE(gChannels[TEGRA_HOST1X_CHANNEL_DISPLAY].lock, gChannels[TEGRA_HOST1X_CHANNEL_VIC].lock);
    IOLockLock(gChannels[TEGRA_HOST1X_CHANNEL_VIC].lock);
    const uint32_t before = doorbell(TEGRA_HOST1X_CHANNEL_DISPLAY);
    ASSERT_NE(job(TEGRA_HOST1X_CHANNEL_DISPLAY, 1, 1), nullptr);
    EXPECT_EQ(doorbell(TEGRA_HOST1X_CHANNEL_DISPLAY), before + 8);
    IOLockUnlock(gChannels[TEGRA_HOST1X_CHANNEL_VIC].lock);
}
