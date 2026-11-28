// SPDX-License-Identifier: BSD-2-Clause-Patent
// The SMMU's statistics (SwitchSmmuRegisters.h statsOn, stats): turning the counters on keeps every other bit of the
// TLB's and PTC's configuration, and nothing is written once both are on.
#include <gtest/gtest.h>

#include <map>
#include <utility>
#include <vector>

#include "../SwitchSmmuRegisters.h"

namespace {
struct Registers {
    std::map<uint32_t, uint32_t> values;
    std::vector<std::pair<uint32_t, uint32_t>> writes;

    uint32_t read(uint32_t offset)
    {
        return values[offset];
    }

    void write(uint32_t offset, uint32_t value)
    {
        writes.emplace_back(offset, value);
        values[offset] = value;
    }
};
}  // namespace

TEST(SmmuStats, TheCountersGoOnWithTheConfigurationKept)
{
    Registers io;
    io.values = {{0x14, 0x30000030}, {0x18, 0x28000f3f}};
    EXPECT_FALSE(switch_smmu::statsOn(io)) << "off at reset";
    EXPECT_EQ(io.writes, (std::vector<std::pair<uint32_t, uint32_t>>{{0x14, 0xb0000030}, {0x18, 0xa8000f3f}}));
    io.writes.clear();
    EXPECT_TRUE(switch_smmu::statsOn(io));
    EXPECT_TRUE(io.writes.empty());
    io.values[0x1f0] = 7;
    io.values[0x1f4] = 3;
    io.values[0x1f8] = 5;
    io.values[0x1fc] = 1;
    const switch_smmu::Stats s = switch_smmu::stats(io);
    EXPECT_EQ(s.tlbHit, 7u);
    EXPECT_EQ(s.tlbMiss, 3u);
    EXPECT_EQ(s.ptcHit, 5u);
    EXPECT_EQ(s.ptcMiss, 1u);
}
