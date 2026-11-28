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
    uint32_t maskedOffset = UINT32_MAX;
    uint32_t writeMask = UINT32_MAX;

    uint32_t read(uint32_t offset)
    {
        return words[offset / 4];
    }

    void write(uint32_t offset, uint32_t value)
    {
        writes.emplace_back(offset, value);
        if (offset == refused)
            return;
        if (offset == maskedOffset)
            value &= writeMask;
        words[offset / 4] = value;
    }
};

class SmmuRestore : public testing::Test {
public:
    Registers io;
    tegra_smmu_attachments attachments{};
    tegra_smmu_attachment display = tegra_smmu_attachment_of(126, 0x8000800, 0x240, 0x228, 7, false);
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
    const tegra_smmu_attachment dev = tegra_smmu_attachment_of(121, 0x8001800, 0x28c, 0x230, 3u << 12, true);
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
    tegra_smmu_attachment read = tegra_smmu_attachment_of(118, 0xe009a000, 0x238, 0x228, 1u << 14, false);
    tegra_smmu_attachment write = tegra_smmu_attachment_of(118, read.ptb, read.asidReg, 0x22c, 1u << 17, false);
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
    const tegra_smmu_attachment gpu0 =
        tegra_smmu_attachment_of(117, 0x8000900, 0xaac, 0x230, (1u << 24) | (1u << 25), false);
    const tegra_smmu_attachment gpu1 =
        tegra_smmu_attachment_of(117, 0x8000900, 0xaac, 0xb98, (1u << 8) | (1u << 9), false);
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

// GPUB (MC 0xaac) as GM20B's address space: RAM below 4 GiB, RAM above it, the compaction window and the fault ASID,
// one per va[33:32]; each directory programmed once, each ASID flushed.
TEST_F(SmmuRestore, ASplitAttachmentNamesAnAsidPerFieldAndProgramsEachDirectoryOnce)
{
    tegra_smmu_attachment gpub = tegra_smmu_attachment_of(115, 0x80001001, 0xaac, 0x230, 3u << 24, false);
    gpub.alwaysTranslated = true;
    const uint32_t upperAsid[3] = {114, 117, 116};
    const uint32_t upperPtb[3] = {0x80001002, 0x80001003, 0x80001004};
    for (uint32_t i = 0; i < 3; ++i) {
        gpub.upperAsid[i] = upperAsid[i];
        gpub.upperPtb[i] = upperPtb[i];
    }
    io.words[0x230 / 4] = gpub.translationBits;
    ASSERT_EQ(switch_smmu::program(io, gpub, line, sizeof(line)), 0) << line;
    EXPECT_EQ(io.words[0xaac / 4], (1u << 31) | 115u | 114u << 8 | 117u << 16 | 116u << 24);
    std::vector<uint32_t> directories;
    std::vector<uint32_t> flushed;
    for (const auto &[offset, value] : io.writes) {
        if (offset == switch_smmu::kPtbData)
            directories.push_back(value);
        if (offset == switch_smmu::kTlbFlush)
            flushed.push_back(value >> 24 & 0x7f);
    }
    EXPECT_EQ(directories, (std::vector<uint32_t>{0x80001001, 0x80001002, 0x80001003, 0x80001004}));
    EXPECT_EQ(flushed, (std::vector<uint32_t>{115, 114, 117, 116, 115, 114, 117, 116}));
}

TEST_F(SmmuRestore, AWideAttachmentProgramsItsOneDirectoryOnce)
{
    const tegra_smmu_attachment dev = tegra_smmu_attachment_of(121, 0x8001800, 0x28c, 0x230, 3u << 12, true);
    io.words[dev.translationReg / 4] = dev.translationBits;
    ASSERT_EQ(switch_smmu::program(io, dev, line, sizeof(line)), 0) << line;
    unsigned directories = 0;
    for (const auto &[offset, value] : io.writes)
        directories += offset == switch_smmu::kPtbData ? 1u : 0u;
    EXPECT_EQ(directories, 1u);
}

// A detach cannot put GPUB in bypass (its enable reads 1): every field goes to the fault ASID instead.
TEST_F(SmmuRestore, ADetachParksAnAlwaysTranslatedSwgroupOnTheFaultAsid)
{
    tegra_smmu_attachment gpub = tegra_smmu_attachment_of(115, 0x80001001, 0xaac, 0x230, 3u << 24, false);
    gpub.alwaysTranslated = true;
    EXPECT_EQ(switch_smmu::parked(gpub, 116), 0xf4747474u);
    EXPECT_EQ(switch_smmu::parked(display, 116), 0u) << "a swgroup that can bypass goes back to bypass";
}

TEST_F(SmmuRestore, PartialSwgroupReadbackRollsBackToPreviousValue)
{
    tegra_smmu_attachment sd = tegra_smmu_attachment_of(113, 0x80002001, 0xa94, 0x234, (1u << 0) | (1u << 4), false);
    const uint32_t upperAsid[3] = {112, 116, 116};
    const uint32_t upperPtb[3] = {0x80002002, 0x80002003, 0x80002003};
    for (uint32_t i = 0; i < 3; ++i) {
        sd.upperAsid[i] = upperAsid[i];
        sd.upperPtb[i] = upperPtb[i];
    }
    io.words[0x234 / 4] = sd.translationBits;
    io.maskedOffset = 0xa94;
    io.writeMask = 0x8000007fu;
    EXPECT_LT(switch_smmu::program(io, sd, line, sizeof(line)), 0);
    EXPECT_EQ(io.words[0xa94 / 4], 0u) << "must roll back to bypass rather than leaving translation on";
}
}
