// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include "sd_card_model.h"

using namespace switch_sd_test;

TEST(SdFilesVolume, BootCardIdentitySurvivesOfflineAndAbsentSharedPartitions)
{
    Card card;
    auto *record = card.page.data() + SWITCH_SD_CARD_OFFSET;
    put(record + 0x54, 0x32333435);
    put(record + 0x58, 0x36123456);
    put(record + 0x5c, 0x789abcff);
    put(record + 0xf0, switch_sd_crc32(record, 0xf0));
    const std::array<uint8_t, 16> expected = {0x1b, 0x53, 0x4d, 0x31, 0x32, 0x33, 0x34, 0x35,
                                              0x36, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0x00};
    ASSERT_EQ(card.select(), SWITCH_GPT_OK);
    std::array<uint8_t, 16> cid{};
    card.sectors[fatFirst][510] = 0;
    ASSERT_EQ(card.select(), SWITCH_GPT_PROBE);
    ASSERT_TRUE(switch_sd_handoff_cid(card.page.data(), cid.data()));
    EXPECT_EQ(cid, expected);

    memset(card.sectors[2].data(), 0, 128);
    card.syncTables();
    ASSERT_NE(card.select(), SWITCH_GPT_OK);
    cid.fill(0);
    ASSERT_TRUE(switch_sd_handoff_cid(card.page.data(), cid.data()));
    EXPECT_EQ(cid, expected);

    card.page[SWITCH_SD_REFERENCE_COMMIT_OFFSET] = 0;
    EXPECT_FALSE(switch_sd_handoff_cid(card.page.data(), cid.data()));
    EXPECT_EQ(cid, expected);
}

TEST(SdFilesVolume, SelectsTheHekateWindowAndFirmwareIdentity)
{
    Card c;
    ASSERT_EQ(c.select(), SWITCH_GPT_OK);
    EXPECT_EQ(c.slice.first_sector, fatFirst);
    EXPECT_FALSE(c.slice.exfat);
    EXPECT_EQ(c.slice.sector_count, fatSize);
    EXPECT_EQ(c.slice.unique_guid[0], 2);
    EXPECT_EQ(c.slice.cid[0], 0x1b);
    EXPECT_EQ(c.slice.cid[1], 0x53);
    for (uint8_t mirror : {0x07, 0x0c}) {
        for (unsigned shift : {9u, 12u}) {
            c.exfat(mirror, shift);
            EXPECT_EQ(c.select(), SWITCH_GPT_OK);
            EXPECT_TRUE(c.slice.exfat);
        }
    }
}

TEST(SdFilesVolume, RefusesDamagedTablesRootMismatchAndWrongCard)
{
    Card c;
    c.slice.first_sector = 123;
    EXPECT_NE(c.select(rootFirst + 1), SWITCH_GPT_OK);
    c.sectors[capacity - 33][16]++;
    EXPECT_EQ(c.select(), SWITCH_GPT_MISMATCH);
    c.syncTables();
    c.sectors[0][100]++;
    EXPECT_EQ(c.select(), SWITCH_GPT_MISMATCH);
    c.page[0x2c] = 0;
    c.reads = 0;
    EXPECT_NE(c.select(), SWITCH_GPT_OK);
    EXPECT_EQ(c.reads, 0u);
    EXPECT_EQ(c.slice.first_sector, 123u);
}

TEST(SdFilesVolume, RequiresAuthorizedBasicDataPartition)
{
    {
        Card c;
        c.sectors[2][0] ^= 1;
        c.syncTables();
        EXPECT_EQ(c.select(), SWITCH_GPT_NOT_FOUND);
    }
    for (unsigned bit : {1u, 60u, 62u, 63u}) {
        Card c;
        put(c.sectors[2].data() + 48, UINT64_C(1) << bit, 8);
        c.syncTables();
        EXPECT_EQ(c.select(), SWITCH_GPT_NOT_FOUND);
    }
    Card c;
    put(c.sectors[2].data() + 160, fatFirst, 8);
    put(c.sectors[2].data() + 168, fatFirst + 22, 8);
    c.syncTables();
    EXPECT_EQ(c.select(), SWITCH_GPT_OVERLAP);
}

TEST(SdFilesVolume, SelectsHandPartitionedPureGptInAnyOrderOrName)
{
    constexpr uint64_t card64G = UINT64_C(128) << 20;
    constexpr uint32_t efiFirst = 40, efiSize = 409600;
    constexpr uint32_t iosFirst = 409640, iosSize = 33554432;
    constexpr uint32_t dataFirst = 33964072, dataSize = 1048576;

    for (uint32_t protectiveCount : {static_cast<uint32_t>(card64G - 1), UINT32_MAX}) {
        for (bool useExfat : {true, false}) {
            for (const char *name : {"MySD", ""}) {
                Card c;
                c.setCapacity(card64G);
                c.setProtectiveMbr(protectiveCount);
                c.clearEntries(4);
                c.setEntry(0, efiType, 10, efiFirst, efiSize, "EFI System Partition");
                c.setEntry(1, apfsType, 11, iosFirst, iosSize, "iOS");
                c.setEntry(2, basicType, 12, dataFirst, dataSize, name);
                c.syncTables();
                writeFat32Vbr(c.sectors[efiFirst], efiSize);
                if (useExfat)
                    writeExfatVbr(c.sectors[dataFirst], dataSize);
                else
                    writeFat32Vbr(c.sectors[dataFirst], dataSize);
                ASSERT_EQ(switch_sd_files_select(Card::read, &c, c.page.data(), iosFirst, iosSize, &c.work, &c.slice),
                          SWITCH_GPT_OK);
                EXPECT_EQ(c.slice.first_sector, dataFirst);
                EXPECT_EQ(c.slice.sector_count, dataSize);
                EXPECT_EQ(c.slice.exfat, useExfat);
                EXPECT_EQ(c.slice.unique_guid[0], 12);
            }
        }
    }

    Card dataBeforeRoot;
    dataBeforeRoot.setCapacity(card64G);
    dataBeforeRoot.setProtectiveMbr(static_cast<uint32_t>(card64G - 1));
    dataBeforeRoot.clearEntries(2);
    dataBeforeRoot.setEntry(0, basicType, 15, 2048, dataSize, "SD Card");
    dataBeforeRoot.setEntry(1, apfsType, 16, 2048 + dataSize, iosSize, "iOS");
    dataBeforeRoot.syncTables();
    writeExfatVbr(dataBeforeRoot.sectors[2048], dataSize);
    ASSERT_EQ(switch_sd_files_select(Card::read, &dataBeforeRoot, dataBeforeRoot.page.data(), 2048 + dataSize, iosSize,
                                     &dataBeforeRoot.work, &dataBeforeRoot.slice),
              SWITCH_GPT_OK);
    EXPECT_EQ(dataBeforeRoot.slice.first_sector, 2048u);
    EXPECT_EQ(dataBeforeRoot.slice.sector_count, dataSize);
    EXPECT_TRUE(dataBeforeRoot.slice.exfat);

    Card sparse;
    sparse.setCapacity(card64G);
    sparse.setProtectiveMbr(static_cast<uint32_t>(card64G - 1));
    sparse.clearEntries(8);
    sparse.setEntry(4, basicType, 20, iosFirst, iosSize, "iOS");
    sparse.setEntry(7, basicType, 21, dataFirst, dataSize, "Storage");
    sparse.syncTables();
    writeFat32Vbr(sparse.sectors[iosFirst], iosSize);
    writeExfatVbr(sparse.sectors[dataFirst], dataSize);
    ASSERT_EQ(
        switch_sd_files_select(Card::read, &sparse, sparse.page.data(), iosFirst, iosSize, &sparse.work, &sparse.slice),
        SWITCH_GPT_OK);
    EXPECT_EQ(sparse.slice.first_sector, dataFirst);
    EXPECT_EQ(sparse.slice.unique_guid[0], 21);
    EXPECT_TRUE(sparse.slice.exfat);

    Card efiOnly;
    efiOnly.setCapacity(card64G);
    efiOnly.setProtectiveMbr(static_cast<uint32_t>(card64G - 1));
    efiOnly.clearEntries(2);
    efiOnly.setEntry(0, efiType, 30, efiFirst, efiSize, "EFI System Partition");
    efiOnly.setEntry(1, apfsType, 31, iosFirst, iosSize, "iOS");
    efiOnly.syncTables();
    writeFat32Vbr(efiOnly.sectors[efiFirst], efiSize);
    EXPECT_EQ(switch_sd_files_select(Card::read, &efiOnly, efiOnly.page.data(), iosFirst, iosSize, &efiOnly.work,
                                     &efiOnly.slice),
              SWITCH_GPT_NOT_FOUND);
}

TEST(SdFilesVolume, SkipsNonFatBasicDataAndDisambiguatesMultipleVolumes)
{
    constexpr uint32_t secondFirst = 700000, secondSize = 524288;
    constexpr uint32_t thirdFirst = 1400000, thirdSize = 524288;
    constexpr uint32_t fourthFirst = 2100000, fourthSize = 524288;

    Card mixed;
    mixed.setProtectiveMbr(UINT32_MAX);
    mbrEntry(mixed.sectors[0].data() + 462, 0x83, thirdFirst, thirdSize);
    mixed.handoffMbr();
    mixed.clearEntries(5);
    mixed.setEntry(0, apfsType, 1, rootFirst, rootSize, "iOS");
    mixed.setEntry(1, basicType, 2, fatFirst, fatSize, "Windows");
    mixed.setEntry(2, basicType, 3, secondFirst, secondSize, "Hidden", UINT64_C(1) << 62);
    mixed.setEntry(3, basicType, 4, thirdFirst, thirdSize, "OldLinux");
    mixed.setEntry(4, basicType, 5, fourthFirst, fourthSize, "Shared");
    mixed.syncTables();
    mixed.sectors[fatFirst].fill(0);
    memcpy(mixed.sectors[fatFirst].data() + 3, "NTFS    ", 8);
    put(mixed.sectors[fatFirst].data() + 510, 0xaa55, 2);
    writeFat32Vbr(mixed.sectors[secondFirst], secondSize);
    mixed.sectors[thirdFirst].fill(0);
    writeExfatVbr(mixed.sectors[fourthFirst], fourthSize);
    ASSERT_EQ(mixed.select(), SWITCH_GPT_OK);
    EXPECT_EQ(mixed.slice.first_sector, fourthFirst);
    EXPECT_EQ(mixed.slice.unique_guid[0], 5);
    EXPECT_TRUE(mixed.slice.exfat);

    Card pair;
    pair.setProtectiveMbr(UINT32_MAX);
    pair.clearEntries(3);
    pair.setEntry(0, apfsType, 1, rootFirst, rootSize, "iOS");
    pair.setEntry(1, basicType, 2, fatFirst, fatSize, "First");
    pair.setEntry(2, basicType, 3, secondFirst, secondSize, "Second");
    pair.syncTables();
    writeFat32Vbr(pair.sectors[fatFirst], fatSize);
    writeExfatVbr(pair.sectors[secondFirst], secondSize);
    pair.slice.first_sector = 777;
    EXPECT_EQ(pair.select(), SWITCH_GPT_AMBIGUOUS);
    EXPECT_EQ(pair.slice.first_sector, 777u);

    pair.setEntry(2, basicType, 3, secondFirst, secondSize, "hos_data");
    pair.syncTables();
    ASSERT_EQ(pair.select(), SWITCH_GPT_OK);
    EXPECT_EQ(pair.slice.first_sector, secondFirst);
    EXPECT_TRUE(pair.slice.exfat);

    pair.setEntry(1, basicType, 2, fatFirst, fatSize, "hos_data");
    pair.syncTables();
    EXPECT_EQ(pair.select(), SWITCH_GPT_AMBIGUOUS);

    pair.setEntry(1, basicType, 2, fatFirst, fatSize, "First");
    pair.setEntry(2, basicType, 3, secondFirst, secondSize, "Second");
    pair.syncTables();
    mbrEntry(pair.sectors[0].data() + 462, 0x07, secondFirst, secondSize);
    pair.handoffMbr();
    ASSERT_EQ(pair.select(), SWITCH_GPT_OK);
    EXPECT_EQ(pair.slice.first_sector, secondFirst);
    EXPECT_TRUE(pair.slice.exfat);

    Card badMbr;
    badMbr.setProtectiveMbr(UINT32_MAX);
    put(badMbr.sectors[0].data() + 454, 2);
    badMbr.handoffMbr();
    EXPECT_EQ(badMbr.select(), SWITCH_GPT_MBR);

    badMbr.setProtectiveMbr(UINT32_MAX);
    badMbr.sectors[0][446] = 0x01;
    badMbr.handoffMbr();
    EXPECT_EQ(badMbr.select(), SWITCH_GPT_MBR);
}

TEST(SdFilesVolume, RejectsMbrAliasesAndWrongVolumeType)
{
    for (uint32_t offset : {0u, 1u}) {
        Card c;
        mbrEntry(c.sectors[0].data() + 478, 0xe0, fatFirst + offset, fatSize);
        c.handoffMbr();
        EXPECT_EQ(c.select(), SWITCH_GPT_MBR);
    }
    Card c;
    c.sectors[0][450] = 0x0b;
    c.handoffMbr();
    EXPECT_EQ(c.select(), SWITCH_GPT_OK);
    c.sectors[0][450] = 7;
    c.handoffMbr();
    EXPECT_EQ(c.select(), SWITCH_GPT_PROBE);
    c.exfat(0x0b);
    EXPECT_EQ(c.select(), SWITCH_GPT_PROBE);
    c.sectors[0][450] = 12;
    put(c.sectors[0].data() + 458, fatSize - 1);
    c.handoffMbr();
    EXPECT_EQ(c.select(), SWITCH_GPT_MBR);
}

TEST(SdFilesVolume, BoundsMetadataInLogicalSectors)
{
    Card c;
    put(c.sectors[fatFirst].data() + 32, fatSize + 1);
    EXPECT_EQ(c.select(), SWITCH_GPT_PROBE);
    put(c.sectors[fatFirst].data() + 32, fatSize);
    put(c.sectors[fatFirst].data() + 36, 1);
    EXPECT_EQ(c.select(), SWITCH_GPT_PROBE);
    for (unsigned offset : {48u, 84u, 88u, 92u, 96u, 108u, 109u, 110u, 511u}) {
        c.exfat(7);
        memset(c.sectors[fatFirst].data() + offset, 0xff, 1);
        if (offset == 84 || offset == 88)
            put(c.sectors[fatFirst].data() + offset, 0xffffff00);
        if (offset == 92 || offset == 96)
            put(c.sectors[fatFirst].data() + offset, UINT32_MAX);
        EXPECT_EQ(c.select(), SWITCH_GPT_PROBE) << offset;
    }
    c.exfat(7);
    put(c.sectors[fatFirst].data() + 92, 1);
    EXPECT_EQ(c.select(), SWITCH_GPT_PROBE);
    c.exfat(7, 12);
    put(c.sectors[fatFirst].data() + 72, fatSize, 8);
    EXPECT_EQ(c.select(), SWITCH_GPT_PROBE);
}

TEST(SdFilesVolume, WriteProtectedCardNeverPublishesAWritableWindow)
{
    for (uint32_t bit : {0x1000u, 0x2000u}) {
        Card c;
        uint8_t *r = c.page.data() + SWITCH_SD_CARD_OFFSET;
        put(r + 0x4c, bit);
        put(r + 0xf0, switch_sd_crc32(r, 0xf0));
        EXPECT_NE(c.select(), SWITCH_GPT_OK);
        EXPECT_EQ(c.reads, 0u);
    }
}
