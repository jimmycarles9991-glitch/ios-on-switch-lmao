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

class SmmuRestore : public testing::Test {
public:
    Registers io;
    tegra_smmu_attachments attachments{};
    tegra_smmu_attachment display{126, 0x8000800, 0x240, 0x228, 7, false};
    char line[160]{};

protected:
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
    io.words[display.translationReg / 4] = (1u << 25) | display.translationBits;
    ASSERT_EQ(restore(), 0);
    EXPECT_EQ(io.words[0x20 / 4], display.ptb);
    EXPECT_EQ(io.words[display.asidReg / 4], (1u << 31) | display.asid);
    for (const auto &[offset, value] : io.writes)
        EXPECT_NE(offset, display.translationReg) << "the translation enables are the secure world's";
    unsigned flushes = 0;
    for (const auto &[offset, value] : io.writes) {
        if (offset == switch_smmu::kTlbFlush) {
            ++flushes;
            EXPECT_EQ(value, (1u << 31) | (126u << 24));
        }
        if (offset == switch_smmu::kPtcFlush)
            EXPECT_EQ(value, 1u << 3) << "flush all, PTC_NO_WAIT_IDLE_FLUSH left at its reset 1";
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
    io.words[display.translationReg / 4] = display.translationBits;
    io.refused = display.asidReg;
    EXPECT_LT(restore(), 0);
    EXPECT_EQ(io.words[display.asidReg / 4], 0u);
}

// A 34-bit client's swgroup (XUSB_DEV here) gets its ASID in all four fields, one per va[33:32], and is restored so.
TEST_F(SmmuRestore, WideSwgroupGetsTheAsidInEveryField)
{
    const tegra_smmu_attachment dev{121, 0x8001800, 0x28c, 0x230, 3u << 12, true};
    ASSERT_TRUE(tegra_smmu_attachments_note(&attachments, &dev));
    io.words[dev.translationReg / 4] = dev.translationBits;
    ASSERT_EQ(switch_smmu::restore(io, attachments, 121, 0x8001800, 0x28c, line, sizeof(line)), 0) << line;
    EXPECT_EQ(io.words[0x28c / 4], 0xf979'7979u);
    EXPECT_EQ(switch_smmu::selection(display), (1u << 31) | 126u);
}

TEST_F(SmmuRestore, ClientTranslationBitsOffDoNotEnableTheSwgroup)
{
    io.words[display.translationReg / 4] = display.translationBits & ~1u;
    EXPECT_LT(restore(), 0);
    EXPECT_EQ(io.words[display.asidReg / 4], 0u);
}

TEST_F(SmmuRestore, BothAfiClientsAreRequiredWithoutWritingSecureRegisters)
{
    tegra_smmu_attachment read{118, 0xe009a000, 0x238, 0x228, 1u << 14, false};
    tegra_smmu_attachment write{118, read.ptb, read.asidReg, 0x22c, 1u << 17, false};
    ASSERT_TRUE(tegra_smmu_attachments_note(&attachments, &read));
    ASSERT_TRUE(tegra_smmu_attachments_note(&attachments, &write));
    io.words[read.translationReg / 4] = (1u << 5) | read.translationBits;
    io.words[write.translationReg / 4] = (1u << 11) | write.translationBits;
    ASSERT_EQ(restore(read.asid, read.ptb, read.asidReg), 0);
    EXPECT_EQ(io.words[read.asidReg / 4], (1u << 31) | read.asid);
    EXPECT_EQ(io.words[display.asidReg / 4], 0u);
    for (const auto &[offset, value] : io.writes) {
        EXPECT_NE(offset, read.translationReg);
        EXPECT_NE(offset, write.translationReg);
    }
    io.writes.clear();
    io.words[read.asidReg / 4] = 0;
    io.words[write.translationReg / 4] = 1u << 11;
    EXPECT_LT(restore(read.asid, read.ptb, read.asidReg), 0);
    EXPECT_EQ(io.words[read.asidReg / 4], 0u);
}

TEST_F(SmmuRestore, ConflictingRetainedClientsCannotProgramADirectory)
{
    auto conflicting = display;
    conflicting.ptb++;
    attachments.entry[attachments.count++] = conflicting;
    EXPECT_LT(restore(), 0);
    EXPECT_TRUE(io.writes.empty());
}

TEST_F(SmmuRestore, BothGpuTranslationRegistersAreRequiredWithoutWritingSecureRegisters)
{
    const tegra_smmu_attachment gpu0{117, 0x8000900, 0xaac, 0x230, (1u << 24) | (1u << 25), false};
    const tegra_smmu_attachment gpu1{117, 0x8000900, 0xaac, 0xb98, (1u << 8) | (1u << 9), false};
    ASSERT_TRUE(tegra_smmu_attachments_note(&attachments, &gpu0));
    ASSERT_TRUE(tegra_smmu_attachments_note(&attachments, &gpu1));
    io.words[0x230 / 4] = gpu0.translationBits;
    io.words[0xb98 / 4] = gpu1.translationBits;
    ASSERT_EQ(restore(117, 0x8000900, 0xaac), 0);
    EXPECT_EQ(io.words[0xaac / 4], (1u << 31) | 117u);
    for (const auto &[offset, value] : io.writes) {
        EXPECT_NE(offset, 0x230u);
        EXPECT_NE(offset, 0xb98u);
    }
    io.writes.clear();
    io.words[0xaac / 4] = 0;
    io.words[0xb98 / 4] = 0;
    EXPECT_LT(restore(117, 0x8000900, 0xaac), 0);
    EXPECT_EQ(io.words[0xaac / 4], 0u);
}
}
