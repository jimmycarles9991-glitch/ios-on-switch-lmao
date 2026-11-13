// SPDX-License-Identifier: MIT
#include "raw_kernel_model.h"
#include "../raw/runtime/raw_channel_recovery.h"
#include <gtest/gtest.h>
#include <string>

namespace {

using kernel_model::model;

constexpr uint32_t kNoFence = 0;
constexpr uint32_t kEngines = TegraGM20BEngine3D | TegraGM20BEngineCopy;
constexpr uint32_t kMmuFault = 1, kGrException = 2;

// raw::Device's channel over the model: its control memory, its channel, fence and last seqno, made as Device::reopen
// makes them, with Device::newContext's job (the copy engine zeroes the interlock tickets from the zero page)
class ChannelRecovery : public testing::Test {
protected:
    raw::Kernel kernel;
    raw::Bo zero, tickets, root;
    uint32_t channel = 0, last = 0;
    const volatile uint32_t *fence = &kNoFence;

    void SetUp() override
    {
        model = {};
        std::string err;
        ASSERT_TRUE(kernel.open(err)) << err;
        ASSERT_TRUE(kernel.allocControl(0x10000, zero, raw::ControlUse::ZeroPage));
        ASSERT_TRUE(kernel.allocControl(0x1000, tickets, raw::ControlUse::InterlockTickets));
        ASSERT_TRUE(kernel.allocControl(0x1000, root, raw::ControlUse::RootTable));
    }

    bool submit(uint32_t on, const raw::SurfaceList &uses, uint32_t &seqno)
    {
        raw::KernelJob job;
        job.channel = on;
        job.uses = uses.uses().data();
        job.useCount = uint32_t(uses.uses().size());
        return kernel.submitJob(job, seqno);
    }

    bool make()
    {
        return raw::makeChannel(kernel, kEngines, channel, fence, last, &kNoFence,
                                [this](uint32_t made, uint32_t &seqno) {
                                    raw::SurfaceList uses;
                                    uses.add(zero.handle, false);
                                    uses.add(tickets.handle, true);
                                    return submit(made, uses, seqno);
                                });
    }

    // A command buffer's job, its control memory named as CommandBuffer::commit names it
    uint32_t job()
    {
        raw::SurfaceList uses;
        uses.add(zero.handle, false);
        uses.add(root.handle, true);
        uses.add(tickets.handle, true);
        uint32_t seqno = 0;
        EXPECT_TRUE(submit(channel, uses, seqno));
        last = seqno;
        return seqno;
    }

    bool done(uint32_t seqno) const
    {
        return fence == &kNoFence || int32_t(*fence - seqno) >= 0;
    }

    raw::KillRange record() const
    {
        const auto *f = reinterpret_cast<const volatile TegraGM20BAbiFence *>(fence);
        return {f->killed_first, f->killed_last, f->error};
    }
};

// R2: NVK holds slot 0 and makes a channel in the slot the reopen freed, so the reopen's channel lands in a slot whose
// fence is far behind the dead channel's last seqno; the kernel refuses its context
TEST_F(ChannelRecovery, AReopenWhoseContextTheKernelRefusesLeavesNoChannelBehind)
{
    model.slots[0].foreign = true;
    ASSERT_TRUE(make());
    for (int i = 0; i < 5; ++i)
        job();
    model.kill(channel, kGrException);
    model.foreignTakesFreedSlot = true;
    kernel.closeChannel(channel);
    model.refuse = 1;
    EXPECT_FALSE(make());
    EXPECT_EQ(channel, 0u);
    EXPECT_EQ(fence, &kNoFence);
    EXPECT_TRUE(done(last)) << "nothing the device submitted may wait on a channel that never runs";
    EXPECT_EQ(model.liveChannels(), 0u) << "the channel the kernel refused the context of was left open";
    ASSERT_TRUE(make());
    EXPECT_TRUE(done(last));
    EXPECT_TRUE(done(job()));
}

// R6: a channel killed with two jobs in flight; the device keeps its record and makes the next channel before the
// completion thread waits for the jobs the dead channel ran
TEST_F(ChannelRecovery, ARetiredChannelsSeqnosEndAsItsKillRecordSays)
{
    raw::ChannelGenerations generations;
    ASSERT_TRUE(make());
    generations.opened();
    const uint64_t first = generations.current();
    const uint32_t before = job(), alsoBefore = job();
    model.inFlight = true;
    const uint32_t running = job(), faulting = job();
    model.kill(channel, kMmuFault);
    ASSERT_TRUE(done(faulting));
    const raw::KillRange killed = record();
    EXPECT_EQ(killed.first, running);
    EXPECT_EQ(killed.last, faulting);
    EXPECT_EQ(generations.retire({killed}), first);
    kernel.closeChannel(channel);
    generations.closed(first);
    model.inFlight = false;
    ASSERT_TRUE(make());
    EXPECT_NE(generations.current(), first);
    EXPECT_TRUE(generations.completed(first, before)) << "a job that completed before the kill read as failed";
    EXPECT_TRUE(generations.completed(first, alsoBefore));
    uint32_t error = 0;
    EXPECT_FALSE(generations.completed(first, running, &error));
    EXPECT_EQ(error, kMmuFault);
    EXPECT_FALSE(generations.completed(first, faulting));
}

TEST(ChannelGenerations, AGenerationNoLongerKeptReadsAsFailed)
{
    raw::ChannelGenerations generations;
    generations.opened();
    const uint64_t first = generations.current();
    generations.closed(generations.retire({}));
    for (int i = 0; i < 64; ++i)
        generations.closed(generations.retire({{1, 1, kMmuFault}}));
    EXPECT_FALSE(generations.completed(first, 5));
}

// R6: kIOReturnNotResponding that the kernel's GetStatus contradicts loses no channel; one it says it killed is lost,
// with its cause
TEST_F(ChannelRecovery, NotRespondingMarksLostOnlyAChannelTheKernelKilled)
{
    ASSERT_TRUE(make());
    const uint32_t seqno = job();
    model.notResponding = 1;
    EXPECT_FALSE(kernel.wait(channel, seqno));
    EXPECT_FALSE(kernel.lost(channel)) << "a WaitFence's NotResponding marked a healthy channel lost";
    model.refuse = 1;
    model.refusal = kIOReturnNotResponding;
    uint32_t refused = 0;
    raw::SurfaceList uses;
    EXPECT_FALSE(submit(channel, uses, refused));
    EXPECT_FALSE(kernel.lost(channel)) << "a submission's NotResponding marked a healthy channel lost";
    EXPECT_TRUE(kernel.wait(channel, seqno));
    model.kill(channel, kGrException);
    EXPECT_FALSE(kernel.wait(channel, seqno + 1));
    EXPECT_TRUE(kernel.lost(channel));
    EXPECT_EQ(kernel.lostError(), kGrException);
}

// R3: the killed jobs wrote the root table and the tickets; the kernel takes the next channel's context and jobs with no
// fresh control memory, and none of them reads what the killed jobs left
TEST_F(ChannelRecovery, AKilledJobsControlMemoryServesTheNextChannel)
{
    ASSERT_TRUE(make());
    job();
    model.inFlight = true;
    job();
    model.kill(channel, kMmuFault);
    EXPECT_EQ(model.undefined.count(root.handle), 1u);
    EXPECT_EQ(model.undefined.count(tickets.handle), 1u);
    kernel.closeChannel(channel);
    model.inFlight = false;
    ASSERT_TRUE(make());
    EXPECT_EQ(model.undefined.count(tickets.handle), 0u) << "the new channel's context did not take the tickets over";
    EXPECT_TRUE(done(job()));
    EXPECT_EQ(model.staleReads, 0u);
}

}  // namespace
