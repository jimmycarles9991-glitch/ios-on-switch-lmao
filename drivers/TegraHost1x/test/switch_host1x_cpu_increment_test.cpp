// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include "../SwitchHost1xCpuIncrement.h"

namespace {
struct Registers {
    uint32_t value = 0, id = 170, writes = 0, reads = 0, barriers = 0;
    bool stuck = false;

    uint32_t read(uint32_t offset)
    {
        EXPECT_EQ(offset, 0xf80 + 4 * id);
        ++reads;
        return value;
    }

    void write(uint32_t offset, uint32_t bits)
    {
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

TEST(SwitchHost1xCpuIncrement, OneStepOfItsOwnSyncpoint)
{
    Registers registers;
    registers.value = UINT32_MAX;
    EXPECT_EQ(switch_host1x_cpu_increment(registers, registers.id, 171), 0);
    EXPECT_EQ(registers.value, 0u);
    EXPECT_EQ(registers.writes, 1u);
    EXPECT_EQ(registers.barriers, 1u);
    EXPECT_EQ(registers.reads, 2u);
}

TEST(SwitchHost1xCpuIncrement, ForeignOrUnallocatedSyncpointsAreNotTouched)
{
    Registers registers;
    EXPECT_EQ(switch_host1x_cpu_increment(registers, 9, 171), -22);
    EXPECT_EQ(switch_host1x_cpu_increment(registers, 171, 171), -22);
    EXPECT_EQ(switch_host1x_cpu_increment(registers, 192, 193), -22);
    EXPECT_EQ(registers.writes, 0u);
    EXPECT_EQ(registers.reads, 0u);
}

TEST(SwitchHost1xCpuIncrement, AStepThatDidNotTakeFailsOnceWithoutWritingAgain)
{
    Registers registers;
    registers.stuck = true;
    EXPECT_EQ(switch_host1x_cpu_increment(registers, registers.id, 171), -5);
    EXPECT_EQ(registers.writes, 1u);
}
}  // namespace
