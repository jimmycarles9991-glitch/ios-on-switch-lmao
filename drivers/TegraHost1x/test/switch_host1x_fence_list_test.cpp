// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <functional>
#include <vector>
#include "../SwitchHost1xFenceList.h"

namespace {
// The sync unit's threshold interrupts: a status bit is set when an increment leaves the value at or past the
// threshold, and is written one to clear; the line is any status bit that is enabled.
struct Model {
    uint32_t value[192] = {}, threshold[192] = {}, status[6] = {}, enabled[6] = {};
    uint32_t thresholdWrites = 0;
    std::function<void(uint32_t id)> onValueRead;

    uint32_t read(uint32_t offset)
    {
        if (offset >= 0xe80 && offset < 0xe98)
            return status[(offset - 0xe80) / 4];
        if (offset >= 0xf80 && offset < 0xf80 + 4 * 192) {
            const uint32_t id = (offset - 0xf80) / 4;
            const uint32_t seen = value[id];
            if (onValueRead)
                onValueRead(id);
            return seen;
        }
        ADD_FAILURE() << "read of " << std::hex << offset;
        return 0;
    }

    void write(uint32_t offset, uint32_t bits)
    {
        if (offset >= 0xe80 && offset < 0xe98)
            status[(offset - 0xe80) / 4] &= ~bits;
        else if (offset >= 0xf00 && offset < 0xf18)
            enabled[(offset - 0xf00) / 4] &= ~bits;
        else if (offset >= 0xf20 && offset < 0xf38)
            enabled[(offset - 0xf20) / 4] |= bits;
        else if (offset >= 0x1380 && offset < 0x1380 + 4 * 192) {
            threshold[(offset - 0x1380) / 4] = bits;
            ++thresholdWrites;
        } else
            ADD_FAILURE() << "write of " << std::hex << offset;
    }

    void increment(uint32_t id, uint32_t times = 1)
    {
        for (uint32_t i = 0; i < times; ++i)
            if (switch_host1x_reached(++value[id], threshold[id]))
                status[id / 32] |= 1u << (id % 32);
    }

    bool armed(uint32_t id) const
    {
        return (enabled[id / 32] >> (id % 32)) & 1;
    }

    bool line() const
    {
        for (uint32_t w = 0; w < 6; ++w)
            if (status[w] & enabled[w])
                return true;
        return false;
    }
};

struct Fixture : testing::Test {
    Model hw;
    SwitchHost1xFenceList<Model> list;
    SwitchHost1xExpired done;
    switch_host1x_fence fences[4] = {};

    switch_host1x_fence *fence(uint32_t i, uint32_t syncpt, uint32_t threshold, uint64_t deadline = 0)
    {
        auto *f = &fences[i];
        *f = {};
        f->syncpt = syncpt;
        f->threshold = threshold;
        f->deadline = deadline;
        return f;
    }

    std::vector<uint32_t> expired()
    {
        std::vector<uint32_t> thresholds;
        for (auto *f = done.take(); f; f = f->next)
            thresholds.push_back(f->threshold);
        return thresholds;
    }
};

TEST_F(Fixture, FencesOnOneSyncpointExpireInThresholdOrderWithTheHeadArmed)
{
    list.add(hw, fence(0, 165, 5), done);
    list.add(hw, fence(1, 165, 3), done);
    list.add(hw, fence(2, 165, 4), done);
    EXPECT_TRUE(expired().empty());
    EXPECT_EQ(hw.threshold[165], 3u);
    EXPECT_TRUE(hw.armed(165));

    hw.increment(165, 4);
    ASSERT_TRUE(hw.line());
    list.interrupt(hw, done);
    EXPECT_EQ(expired(), (std::vector<uint32_t>{3, 4}));
    EXPECT_EQ(hw.threshold[165], 5u);
    EXPECT_TRUE(hw.armed(165));
    EXPECT_FALSE(hw.line());

    hw.increment(165);
    list.interrupt(hw, done);
    EXPECT_EQ(expired(), (std::vector<uint32_t>{5}));
    EXPECT_FALSE(hw.armed(165));
    EXPECT_FALSE(hw.line());
}

TEST_F(Fixture, AFenceItsSyncpointAlreadyPassedExpiresAtAddWithoutArming)
{
    hw.value[166] = 10;
    list.add(hw, fence(0, 166, 8), done);
    EXPECT_EQ(expired(), (std::vector<uint32_t>{8}));
    EXPECT_EQ(hw.thresholdWrites, 0u);
    EXPECT_FALSE(hw.armed(166));
}

// The increment lands between the add's first read and the threshold write, so it sets no status bit: the read after
// the enable is what catches it.
TEST_F(Fixture, AnIncrementBeforeTheThresholdWriteIsCaughtByTheReadBack)
{
    bool first = true;
    hw.onValueRead = [&](uint32_t id) {
        if (first && id == 167) {
            first = false;
            hw.value[167] = 7;
        }
    };
    list.add(hw, fence(0, 167, 5), done);
    EXPECT_EQ(expired(), (std::vector<uint32_t>{5}));
    EXPECT_FALSE(hw.armed(167));
    EXPECT_EQ(hw.status[5], 0u);
}

TEST_F(Fixture, RemovingTheHeadArmsTheNextFenceAndTheLastDisarms)
{
    auto *early = fence(0, 168, 3);
    auto *late = fence(1, 168, 5);
    list.add(hw, early, done);
    list.add(hw, late, done);
    EXPECT_TRUE(list.remove(hw, early, done));
    EXPECT_EQ(early->state, SWITCH_HOST1X_FENCE_IDLE);
    EXPECT_EQ(hw.threshold[168], 5u);
    EXPECT_TRUE(hw.armed(168));
    EXPECT_FALSE(list.remove(hw, early, done));
    EXPECT_TRUE(list.remove(hw, late, done));
    EXPECT_FALSE(hw.armed(168));
    EXPECT_TRUE(expired().empty());
}

TEST_F(Fixture, ThresholdsCompareAcrossTheWrap)
{
    hw.value[169] = 0xfffffff0u;
    list.add(hw, fence(0, 169, 2), done);
    list.add(hw, fence(1, 169, 0xfffffffau), done);
    EXPECT_EQ(hw.threshold[169], 0xfffffffau);
    hw.increment(169, 10);
    list.interrupt(hw, done);
    EXPECT_EQ(expired(), (std::vector<uint32_t>{0xfffffffau}));
    hw.increment(169, 8);
    list.interrupt(hw, done);
    EXPECT_EQ(expired(), (std::vector<uint32_t>{2}));
}

TEST_F(Fixture, StatusBitsOfSyncpointsNobodyArmedAreLeftSet)
{
    hw.status[170 / 32] = 1u << (170 % 32);
    list.add(hw, fence(0, 171, 1), done);
    hw.increment(171);
    list.interrupt(hw, done);
    EXPECT_EQ(expired(), (std::vector<uint32_t>{1}));
    EXPECT_EQ(hw.status[170 / 32], 1u << (170 % 32));
}

TEST_F(Fixture, EveryArmedSyncpointWithItsStatusSetIsServedByOneInterrupt)
{
    list.add(hw, fence(0, 160, 1), done);
    list.add(hw, fence(1, 191, 1), done);
    list.add(hw, fence(2, 175, 1), done);
    hw.increment(160);
    hw.increment(191);
    list.interrupt(hw, done);
    EXPECT_EQ(expired().size(), 2u);
    EXPECT_TRUE(hw.armed(175));
}

TEST_F(Fixture, OverdueFencesTimeOutOrGetOneGraceThenAreLost)
{
    uint64_t next = 0;
    list.add(hw, fence(0, 172, 2, 100), done);
    list.add(hw, fence(1, 173, 1, 100), done);
    list.add(hw, fence(2, 174, 1, 300), done);
    hw.value[173] = 1;
    EXPECT_EQ(list.overdue(hw, 150, 50, true, -60, -5, done, &next), nullptr);
    EXPECT_EQ(expired(), (std::vector<uint32_t>{2}));
    EXPECT_EQ(fences[0].status, -60);
    EXPECT_FALSE(hw.armed(172));
    EXPECT_EQ(fences[1].graced, 1u);
    EXPECT_EQ(next, 200u);

    EXPECT_EQ(list.overdue(hw, 200, 50, true, -60, -5, done, &next), &fences[1]);
    EXPECT_EQ(fences[1].state, SWITCH_HOST1X_FENCE_QUEUED);
    EXPECT_EQ(list.overdue(hw, 200, 50, false, -60, -5, done, &next), nullptr);
    EXPECT_EQ(expired(), (std::vector<uint32_t>{1}));
    EXPECT_EQ(fences[1].status, -5);
    EXPECT_EQ(next, 300u);
}

TEST_F(Fixture, UnboundedFencesAreNeverOverdue)
{
    uint64_t next = 0;
    list.add(hw, fence(0, 176, 1), done);
    EXPECT_EQ(list.overdue(hw, UINT64_MAX, 50, true, -60, -5, done, &next), nullptr);
    EXPECT_TRUE(expired().empty());
    EXPECT_EQ(next, 0u);
}

TEST_F(Fixture, RearmAfterAResetArmsEveryPendingSyncpointAndExpiresThePassed)
{
    list.add(hw, fence(0, 177, 3), done);
    list.add(hw, fence(1, 178, 9), done);
    for (auto &word : hw.enabled)
        word = 0;
    hw.value[177] = 3;
    list.rearm(hw, done);
    EXPECT_EQ(expired(), (std::vector<uint32_t>{3}));
    EXPECT_FALSE(hw.armed(177));
    EXPECT_TRUE(hw.armed(178));
    EXPECT_EQ(hw.threshold[178], 9u);
}
}  // namespace
