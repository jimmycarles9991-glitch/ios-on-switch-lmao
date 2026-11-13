// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <cstdio>
#include <vector>
#include "../SwitchSmmuRegisters.h"

namespace {
struct Registers {
    std::array<uint32_t, 1024> words{};
    std::vector<std::pair<uint32_t, uint32_t>> writes;
    uint32_t refused = UINT32_MAX;

    uint32_t read(uint32_t offset)
    {
        return words[offset / 4];
    }

    void write(uint32_t offset, uint32_t value)
    {
        writes.emplace_back(offset, value);
        if (offset != refused)
            words[offset / 4] = value;
    }
};

struct SmmuRestore : testing::Test {
    Registers io;
    tegra_smmu_attachments attachments{};
    tegra_smmu_attachment display{126, 0x8000800, 0x240, 0x228, 7};
    char line[160]{};

    void SetUp() override
    {
        ASSERT_TRUE(tegra_smmu_attachments_note(&attachments, &display));
    }

    int restore(uint32_t asid = 126, uint32_t ptb = 0x8000800, uint32_t group = 0x240)
    {
        return switch_smmu::restore(io, attachments, asid, ptb, group, line, sizeof(line));
    }
};

TEST_F(SmmuRestore, RestoresRetainedDirectoryAndEveryClientAfterFirmwareStateChanged)
{
    io.words[display.asidReg / 4] = (1u << 31) | 5;
    io.words[display.translationReg / 4] = 1u << 25;
    ASSERT_EQ(restore(), 0);
    EXPECT_EQ(io.words[0x20 / 4], display.ptb);
    EXPECT_EQ(io.words[display.asidReg / 4], (1u << 31) | display.asid);
    EXPECT_EQ(io.words[display.translationReg / 4], (1u << 25) | display.translationBits);
    unsigned flushes = 0;
    for (const auto &[offset, value] : io.writes)
        if (offset == switch_smmu::kTlbFlush) {
            ++flushes;
            EXPECT_EQ(value, (1u << 31) | (126u << 24));
        }
    EXPECT_EQ(flushes, 2u);
}

TEST_F(SmmuRestore, StaleOrUnownedIdentityCannotReplaceLiveAttachment)
{
    EXPECT_LT(restore(125), 0);
    EXPECT_LT(restore(126, 0x8000900), 0);
    EXPECT_LT(restore(126, display.ptb, 0x284), 0);
    EXPECT_TRUE(io.writes.empty());
}

TEST_F(SmmuRestore, DetachedIdentityCannotBeResurrected)
{
    tegra_smmu_attachments_forget(&attachments, display.asidReg);
    EXPECT_LT(restore(), 0);
    EXPECT_TRUE(io.writes.empty());
}

TEST_F(SmmuRestore, FailedDirectoryReadbackNeverEnablesTranslation)
{
    io.refused = switch_smmu::kPtbData;
    EXPECT_LT(restore(), 0);
    EXPECT_EQ(io.words[display.asidReg / 4], 0u);
    EXPECT_EQ(io.words[display.translationReg / 4], 0u);
}

TEST_F(SmmuRestore, FailedSwgroupReadbackIsNotReportedAsRestored)
{
    io.refused = display.asidReg;
    EXPECT_LT(restore(), 0);
    EXPECT_EQ(io.words[display.asidReg / 4], 0u);
}

TEST_F(SmmuRestore, FailedClientTranslationReadbackDoesNotEnableTheSwgroup)
{
    io.refused = display.translationReg;
    EXPECT_LT(restore(), 0);
    EXPECT_EQ(io.words[display.asidReg / 4], 0u);
}
}
