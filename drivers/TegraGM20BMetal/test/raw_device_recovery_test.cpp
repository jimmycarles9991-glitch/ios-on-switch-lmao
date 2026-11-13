// SPDX-License-Identifier: MIT
#include "raw_device_harness.h"

namespace {

using kernel_model::model;
constexpr uint32_t kGrException = 2;

class DeviceRecovery : public harness::DeviceTest {
protected:
    raw::Buffer a, b;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createBuffer(4096, a));
        ASSERT_TRUE(dev.createBuffer(4096, b));
    }

    void kill()
    {
        const uint32_t channel = liveChannel();
        ASSERT_NE(channel, 0u);
        model.kill(channel, kGrException);
    }
};

TEST_F(DeviceRecovery, AKillFailsWhatItTookAndTheNextCommandBufferGetsANewChannel)
{
    const uint32_t before = copy(a, b, 4096);
    ASSERT_NE(before, 0u);
    EXPECT_TRUE(dev.wait(before));
    model.inFlight = true;
    const uint32_t running = copy(a, b, 4096);
    ASSERT_NE(running, 0u);
    const uint64_t generation = dev.channelGeneration();
    kill();
    model.inFlight = false;
    EXPECT_FALSE(dev.wait(running, generation));
    const uint32_t after = copy(b, a, 4096);
    ASSERT_NE(after, 0u);
    EXPECT_EQ(model.creates, 2u);
    EXPECT_EQ(model.destroys, 1u);
    EXPECT_NE(dev.channelGeneration(), generation);
    EXPECT_TRUE(dev.wait(after));
}

TEST_F(DeviceRecovery, AReopenTheKernelRefusesLeavesTheDeviceToTryAgain)
{
    ASSERT_NE(copy(a, b, 4096), 0u);
    kill();
    for (kernel_model::Slot &s : model.slots)
        if (!s.uid)
            s.foreign = true;
    model.foreignTakesFreedSlot = true;
    EXPECT_EQ(dev.begin(), nullptr);
    for (kernel_model::Slot &s : model.slots)
        if (s.foreign && !s.uid)
            s.foreign = false;
    const uint32_t after = copy(a, b, 4096);
    ASSERT_NE(after, 0u);
    EXPECT_TRUE(dev.wait(after));
}

// A push ring replaced while the dead channel's work was in flight goes back once the channel is made again, though the new
// channel lands in a slot whose fence is far behind the seqnos the old one gave out
TEST_F(DeviceRecovery, AnOutgrownRingOfADeadChannelIsFreedAtTheNextCommandBuffer)
{
    const uint64_t ring = model.nextHandle;
    ASSERT_NE(copy(a, b, 4096), 0u);
    for (int i = 0; i < 64; i++)
        ASSERT_NE(copy(a, b, 16), 0u);
    model.inFlight = true;
    ASSERT_NE(copy(a, b, 16, 20000), 0u);
    ASSERT_NE(copy(a, b, 16), 0u);
    EXPECT_FALSE(freed(ring));
    kill();
    model.inFlight = false;
    model.foreignTakesFreedSlot = true;
    ASSERT_NE(copy(a, b, 16), 0u);
    EXPECT_EQ(model.creates, 2u);
    ASSERT_NE(copy(a, b, 16), 0u);
    EXPECT_TRUE(freed(ring));
}

TEST_F(DeviceRecovery, AnOutgrownRingOfALiveChannelWaitsForItsWork)
{
    const uint64_t ring = model.nextHandle;
    ASSERT_NE(copy(a, b, 4096), 0u);
    model.inFlight = true;
    ASSERT_NE(copy(a, b, 16, 20000), 0u);
    ASSERT_NE(copy(a, b, 16), 0u);
    ASSERT_NE(copy(a, b, 16), 0u);
    EXPECT_FALSE(freed(ring));
    kernel_model::Slot *slot = liveSlot();
    ASSERT_NE(slot, nullptr);
    model.fence(liveChannel() - 1).completed = slot->submitted;
    model.inFlight = false;
    ASSERT_NE(copy(a, b, 16), 0u);
    EXPECT_TRUE(freed(ring));
}

// An event job checked against one channel is not submitted on the channel made after a kill: the order and the waits'
// producers its caller found are the dead channel's
TEST_F(DeviceRecovery, AnEventJobOfADeadChannelIsOrderedAgainOnTheNewOne)
{
    ASSERT_NE(copy(a, b, 4096), 0u);
    const TegraGM20BAbiEventUse signal = {7, 1};
    const raw::JobEvents events{nullptr, &signal, 0, 1};
    const uint64_t generation = dev.channelGeneration();
    kill();
    uint32_t seqno = 0;
    int result = KERN_SUCCESS;
    EXPECT_FALSE(dev.eventJob(events, seqno, &result, generation));
    EXPECT_EQ(result, kIOReturnNotResponding);
    EXPECT_EQ(model.creates, 2u);
    const size_t jobs = model.jobs.size();
    result = KERN_SUCCESS;
    ASSERT_TRUE(dev.eventJob(events, seqno, &result, dev.channelGeneration()));
    EXPECT_EQ(model.jobs.size(), jobs + 1);
    EXPECT_EQ(model.jobs.back().seqno, seqno);
    EXPECT_TRUE(dev.wait(seqno));
}

TEST_F(DeviceRecovery, AnEventJobWithNoOrderTakesWhicheverChannelIsLive)
{
    ASSERT_NE(copy(a, b, 4096), 0u);
    const TegraGM20BAbiEventUse signal = {7, 1};
    kill();
    uint32_t seqno = 0;
    ASSERT_TRUE(dev.eventJob({nullptr, &signal, 0, 1}, seqno));
    EXPECT_EQ(model.creates, 2u);
    EXPECT_TRUE(dev.wait(seqno));
}

}  // namespace
