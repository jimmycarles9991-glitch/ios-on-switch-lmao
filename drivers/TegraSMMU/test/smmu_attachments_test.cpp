// drivers/TegraSMMU/TegraSMMUAttachments.h on the host: what tegra_smmu_resume programs again after SC7.
#include <gtest/gtest.h>

#include "../TegraSMMUAttachments.h"

namespace {
tegra_smmu_attachment entry(uint32_t asid, uint32_t ptb, uint32_t asidReg)
{
    return {asid, ptb, asidReg, 0x234, 1u << 30, false};
}
}  // namespace

TEST(SmmuAttachments, KeepsOneEntryPerSwgroupInAttachOrder)
{
    tegra_smmu_attachments t = {};
    const auto vic = entry(127, 0x8000a000, 0x284);
    const auto nvjpg = entry(125, 0x8000b000, 0xac0);
    const auto dc = entry(126, 0x8000c000, 0x240);
    ASSERT_TRUE(tegra_smmu_attachments_note(&t, &vic));
    ASSERT_TRUE(tegra_smmu_attachments_note(&t, &nvjpg));
    ASSERT_TRUE(tegra_smmu_attachments_note(&t, &dc));
    ASSERT_EQ(t.count, 3u);
    auto const dcAgain = entry(126, 0x8000d000, 0x240);
    ASSERT_TRUE(tegra_smmu_attachments_note(&t, &dcAgain));
    ASSERT_EQ(t.count, 3u);
    EXPECT_EQ(t.entry[2].ptb, 0x8000d000u);
    EXPECT_EQ(t.entry[0].asidReg, 0x284u);
    EXPECT_EQ(t.entry[1].asidReg, 0xac0u);
}

TEST(SmmuAttachments, DetachDropsOnlyThatSwgroup)
{
    tegra_smmu_attachments t = {};
    const auto vic = entry(127, 1, 0x284);
    const auto nvjpg = entry(125, 2, 0xac0);
    const auto dc = entry(126, 3, 0x240);
    tegra_smmu_attachments_note(&t, &vic);
    tegra_smmu_attachments_note(&t, &nvjpg);
    tegra_smmu_attachments_note(&t, &dc);
    tegra_smmu_attachments_forget(&t, 0xac0);
    ASSERT_EQ(t.count, 2u);
    EXPECT_EQ(t.entry[0].asidReg, 0x284u);
    EXPECT_EQ(t.entry[1].asidReg, 0x240u);
    tegra_smmu_attachments_forget(&t, 0x999);
    EXPECT_EQ(t.count, 2u);
}

TEST(SmmuAttachments, FullTableRefusesANewSwgroupButStillUpdatesAKnownOne)
{
    tegra_smmu_attachments t = {};
    for (uint32_t i = 0; i < TEGRA_SMMU_ATTACHMENTS; ++i) {
        auto const a = entry(100 + i, i, 0x300 + 4 * i);
        ASSERT_TRUE(tegra_smmu_attachments_note(&t, &a));
    }
    auto const extra = entry(1, 1, 0x500);
    EXPECT_FALSE(tegra_smmu_attachments_note(&t, &extra));
    auto const known = entry(100, 0xabc, 0x300);
    EXPECT_TRUE(tegra_smmu_attachments_note(&t, &known));
    EXPECT_EQ(t.entry[0].ptb, 0xabcu);
}

TEST(SmmuAttachments, RetainsBothAfiClientRegistersAndMergesTheirBits)
{
    tegra_smmu_attachments t = {};
    tegra_smmu_attachment read{118, 0xe009a000, 0x238, 0x228, 1u << 14, false};
    tegra_smmu_attachment write{read.asid, read.ptb, read.asidReg, 0x22c, 1u << 17, false};
    ASSERT_TRUE(tegra_smmu_attachments_note(&t, &read));
    ASSERT_TRUE(tegra_smmu_attachments_note(&t, &write));
    read.translationBits |= 1u << 15;
    ASSERT_TRUE(tegra_smmu_attachments_note(&t, &read));
    ASSERT_EQ(t.count, 2u);
    EXPECT_EQ(t.entry[0].translationBits, (1u << 14) | (1u << 15));
    EXPECT_EQ(t.entry[1].translationReg, 0x22cu);
    EXPECT_EQ(t.entry[1].translationBits, 1u << 17);
    write.ptb += 1;
    EXPECT_FALSE(tegra_smmu_attachments_note(&t, &write));
    EXPECT_EQ(t.entry[1].ptb, read.ptb);
    tegra_smmu_attachments_forget(&t, read.asidReg);
    EXPECT_EQ(t.count, 0u);
}
