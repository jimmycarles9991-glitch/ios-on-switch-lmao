// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include "../SwitchHost1xFailureRelease.h"

namespace {
struct Registers {
    uint32_t value = 0, id = 167, writes = 0, reads = 0, barriers = 0;
    bool failedProducerStopped = true, stuck = false;

    uint32_t read(uint32_t offset)
    {
        EXPECT_EQ(offset, 0xf80 + 4 * id);
        ++reads;
        return value;
    }

    void write(uint32_t offset, uint32_t bits)
    {
        EXPECT_TRUE(failedProducerStopped);
        EXPECT_EQ(offset, 0xf60 + 4 * (id / 32));
        EXPECT_EQ(bits, 1u << (id % 32));
        ++writes;
        if (!stuck)
            ++value;
    }

    void barrier()
    {
        ++barriers;
    }
};

TEST(SwitchHost1xRelease, StoppedTwoIncrementProducerReleasesOnlyItsOwnedSyncpoint)
{
    Registers registers;
    registers.value = 41;
    EXPECT_EQ(switch_host1x_release_failed(registers, registers.id, 43, 2, 168), 0);
    EXPECT_EQ(registers.value, 43u);
    EXPECT_EQ(registers.writes, 2u);
    EXPECT_EQ(registers.reads, 2u);
    EXPECT_EQ(registers.barriers, 1u);
}

TEST(SwitchHost1xRelease, PartialReleaseAndCounterWrapStayBounded)
{
    Registers registers;
    registers.value = UINT32_MAX;
    EXPECT_EQ(switch_host1x_release_failed(registers, registers.id, 0, 2, 168), 0);
    EXPECT_EQ(registers.writes, 1u);
    EXPECT_EQ(registers.value, 0u);
}

TEST(SwitchHost1xRelease, AlreadyReachedDoesNotIncrement)
{
    Registers registers;
    registers.value = 45;
    EXPECT_EQ(switch_host1x_release_failed(registers, registers.id, 43, 2, 168), 0);
    EXPECT_EQ(registers.writes, 0u);
    EXPECT_EQ(registers.reads, 1u);
}

TEST(SwitchHost1xRelease, UnallocatedForeignOrExcessDistanceCannotRelease)
{
    Registers registers;
    EXPECT_EQ(switch_host1x_release_failed(registers, 12, 2, 2, 168), -22);
    EXPECT_EQ(switch_host1x_release_failed(registers, 168, 2, 2, 168), -22);
    EXPECT_EQ(switch_host1x_release_failed(registers, registers.id, 3, 2, 168), -22);
    EXPECT_EQ(switch_host1x_release_failed(registers, registers.id, 1, 3, 168), -22);
    EXPECT_EQ(registers.writes, 0u);
}

TEST(SwitchHost1xRelease, FailedHardwareWriteReturnsErrorWithoutPolling)
{
    Registers registers;
    registers.stuck = true;
    EXPECT_EQ(switch_host1x_release_failed(registers, registers.id, 2, 2, 168), -5);
    EXPECT_EQ(registers.writes, 2u);
    EXPECT_EQ(registers.reads, 2u);
}
}  // namespace
