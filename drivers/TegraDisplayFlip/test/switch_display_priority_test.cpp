// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include <vector>
#include "../SwitchDisplayPriority.h"

namespace {
struct Registers {
    std::array<uint32_t, 0x500> words{};
    std::array<uint32_t, 2> assembly{}, armed{}, active{};
    std::vector<uint32_t> writes;
    bool directWrite = false;

    struct Word {
        Registers &regs;
        uint32_t index;

        operator uint32_t() const
        {
            return regs.words[index];
        }

        Word &operator=(uint32_t value)
        {
            regs.writes.push_back(index);
            regs.words[index] = value;
            if (index == 0x403 || index == 0x404) {
                regs.directWrite |= (regs.words[0x40] & 4) != 0;
                regs.assembly[index - 0x403] = value;
            }
            if (index == 0x41 && (value & 0x100))
                regs.armed = regs.assembly;
            if (index == 0x41 && (value & 1))
                regs.active = regs.armed;
            return *this;
        }
    };

    Word operator[](uint32_t index)
    {
        return {*this, index};
    }
};

TEST(DisplayPriority, AllThreeWindowFieldsMatchTegra210Initialization)
{
    Registers regs;
    SwitchDisplayPriority::initialize(regs);
    EXPECT_EQ((regs.active[0] >> 16) & 0x7f, 32u);
    EXPECT_EQ((regs.active[0] >> 8) & 0x7f, 32u);
    EXPECT_EQ(regs.active[0] & 0xff, 32u);
    for (unsigned shift : {0u, 8u, 16u})
        EXPECT_EQ((regs.active[1] >> shift) & 0x3f, 1u);
    EXPECT_EQ(regs.active[0] >> 24, 0u);
    EXPECT_EQ(regs.active[1] >> 24, 0u);
}

TEST(DisplayPriority, FirmwareActiveWriteMuxUsesAssemblyAndIsRestored)
{
    Registers regs;
    regs.words[0x40] = 5;
    regs.words[0x32] = 0x20;
    regs.words[0x43] = 0x454;
    SwitchDisplayPriority::initialize(regs);
    EXPECT_FALSE(regs.directWrite);
    EXPECT_EQ(regs.words[0x40], 5u);
    EXPECT_EQ(regs.words[0x32], 0x20u);
    EXPECT_EQ(regs.words[0x43], 0x454u);
    EXPECT_EQ(regs.active, regs.assembly);
    EXPECT_EQ(regs.writes, (std::vector<uint32_t>{0x40, 0x403, 0x404, 0x41, 0x41, 0x40}));
}

TEST(DisplayPriority, ReinitializationRestoresLostPriorityWithoutWindowChanges)
{
    Registers regs;
    regs.words.fill(0xa5a5a5a5);
    regs.words[0x40] = 1;
    SwitchDisplayPriority::initialize(regs);
    regs.active = {};
    regs.assembly = {};
    regs.armed = {};
    SwitchDisplayPriority::initialize(regs);
    EXPECT_EQ(regs.active[0], 0x202020u);
    EXPECT_EQ(regs.active[1], 0x10101u);
    for (uint32_t i = 0; i < regs.words.size(); ++i) {
        if (i != 0x40 && i != 0x41 && i != 0x403 && i != 0x404)
            EXPECT_EQ(regs.words[i], 0xa5a5a5a5u) << i;
    }
}
}  // namespace
