// SPDX-License-Identifier: BSD-2-Clause-Patent
// A debug kernel's probe resources (built with DEVELOPMENT=1): VIC's second and third channels get their rings and
// arenas only at the module lock probe's first ask and are programmed at every resume after it; the probes' syncpoints
// are one set of four, held by one probe at a time and taken from the pool once.
#include "switch_host1x_submit_harness.h"

namespace {

uint32_t dmaStart(uint32_t channel)
{
    return switch_mmio_read32(gHostRegisters, channel * kChannelStride + CH_DMASTART);
}

bool programmed(uint32_t channel)
{
    for (const auto &[offset, bits] : gRegisterWrites)
        if (offset == channel * kChannelStride + CH_DMASTART)
            return true;
    return false;
}

TEST_F(Host1xSubmit, VicsProbeChannelsAreMadeAtTheProbesFirstAskAndProgrammedAtEveryResumeAfter)
{
    static_assert(TEGRA_HOST1X_CHANNELS == TEGRA_HOST1X_CHANNEL_VIC_THIRD + 1);
    constexpr uint32_t kProbe[] = {TEGRA_HOST1X_CHANNEL_VIC_SECOND, TEGRA_HOST1X_CHANNEL_VIC_THIRD};
    EXPECT_EQ(gAllocated, 2u * (TEGRA_HOST1X_CHANNEL_DISPLAY + 1)) << "bring-up makes the engines' channels alone";
    const switch_dma_buffer *buffers[2 * TEGRA_HOST1X_CHANNELS] = {};
    const uint32_t bootBuffers = switch_host1x_channels_buffers(buffers, 2 * TEGRA_HOST1X_CHANNELS);
    EXPECT_EQ(bootBuffers, 2u * (TEGRA_HOST1X_CHANNEL_DISPLAY + 1));
    for (uint32_t i = 0; i < bootBuffers; ++i) {
        EXPECT_NE(buffers[i]->pa, 0u) << i;
        EXPECT_NE(buffers[i]->size, 0u) << i;
    }
    uint32_t *words = nullptr;
    for (const uint32_t channel : kProbe) {
        EXPECT_EQ(switch_host1x_job_reserve(channel, 4, &words), TEGRA_HOST1X_ENODEV) << channel;
        EXPECT_EQ(dmaStart(channel), 0u) << channel;
    }

    ASSERT_EQ(switch_host1x_probe_channels_take(), TEGRA_HOST1X_OK);
    EXPECT_EQ(gAllocated, 2u * TEGRA_HOST1X_CHANNELS);
    EXPECT_EQ(switch_host1x_channels_buffers(buffers, 2 * TEGRA_HOST1X_CHANNELS), 2u * TEGRA_HOST1X_CHANNELS);
    for (const uint32_t channel : kProbe) {
        EXPECT_EQ(dmaStart(channel), kPushPA + channel * 0x100000u) << channel;
        ASSERT_EQ(switch_host1x_job_reserve(channel, 4, &words), TEGRA_HOST1X_OK) << channel;
        switch_host1x_job_cancel(channel);
    }
    ASSERT_EQ(switch_host1x_probe_channels_take(), TEGRA_HOST1X_OK);
    EXPECT_EQ(gAllocated, 2u * TEGRA_HOST1X_CHANNELS) << "made once";

    tegra_host1x_suspend();
    gRegisterWrites.clear();
    tegra_host1x_resume();
    for (uint32_t channel = 0; channel < TEGRA_HOST1X_CHANNELS; ++channel)
        EXPECT_TRUE(programmed(channel)) << channel;
}

TEST_F(Host1xSubmit, TheProbesShareOneSetOfFourSyncpointsTakenOnce)
{
    uint32_t first[SWITCH_HOST1X_PROBE_SYNCPTS] = {}, again[SWITCH_HOST1X_PROBE_SYNCPTS] = {};
    const uint32_t before = gPool.inUse;
    ASSERT_EQ(switch_host1x_probe_syncpts_claim(first), TEGRA_HOST1X_OK);
    EXPECT_EQ(gPool.inUse, before + SWITCH_HOST1X_PROBE_SYNCPTS);
    for (uint32_t i = 0; i < SWITCH_HOST1X_PROBE_SYNCPTS; ++i) {
        EXPECT_GE(first[i], SwitchHost1xSyncptPool::kFirstHandedOut) << i;
        EXPECT_LT(first[i], SwitchHost1xSyncptPool::kSyncpts) << i;
        for (uint32_t j = 0; j < i; ++j)
            EXPECT_NE(first[i], first[j]) << i << " " << j;
    }
    EXPECT_EQ(switch_host1x_probe_syncpts_claim(again), TEGRA_HOST1X_EBUSY) << "one probe holds it at a time";
    switch_host1x_probe_syncpts_release();
    ASSERT_EQ(switch_host1x_probe_syncpts_claim(again), TEGRA_HOST1X_OK);
    switch_host1x_probe_syncpts_release();
    for (uint32_t i = 0; i < SWITCH_HOST1X_PROBE_SYNCPTS; ++i)
        EXPECT_EQ(again[i], first[i]) << i;
    EXPECT_EQ(gPool.inUse, before + SWITCH_HOST1X_PROBE_SYNCPTS) << "the pool gives the set once";
}

// Every probe that writes host1x streams takes the shared set and its own channels' memory, never the pool or the
// engines' channels.
TEST(ProbeResources, EveryProbeClaimsTheSharedSetAndLetsItGo)
{
    using switch_ios::test::Block;
    using switch_ios::test::CodeText;
    using switch_ios::test::SourceText;
    const std::string mlock = SourceText("../SwitchHost1xMlockProbe.cpp");
    EXPECT_EQ(CodeText(mlock).count("tegra_host1x_syncpt_alloc"), 0u);
    const CodeText run = Block(mlock, "extern \"C\" int switch_host1x_mlock_probe_run(void *out)");
    EXPECT_TRUE(run.before("switch_host1x_probe_channels_take()", "switch_host1x_probe_syncpts_claim(gIds)"));
    EXPECT_TRUE(run.before("switch_host1x_probe_syncpts_claim(gIds)", "runCase(false)"));
    EXPECT_TRUE(run.before("runCase(true)", "switch_host1x_probe_syncpts_release();"));
    EXPECT_EQ(run.count("switch_host1x_probe_syncpts_release();"), 1u);
    EXPECT_EQ(run.count("return 0;"), 4u) << "host1x, the channels or the set refused, and the end after the release";

    const std::string channels = SourceText("../SwitchHost1xChannels.cpp");
    EXPECT_TRUE(Block(channels, "bool switch_host1x_channels_alloc(volatile uint8_t *host, volatile uint8_t *sync, "
                                "IOLock *lock)")
                    .has("for (uint32_t n = 0; n <= TEGRA_HOST1X_CHANNEL_DISPLAY; n++)"));
}

}  // namespace
