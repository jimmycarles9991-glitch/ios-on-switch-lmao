// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <cstring>
#include <map>
#include "../SwitchSdVolumeInternal.h"

namespace {
using Sector = std::array<uint8_t, 512>;
constexpr uint64_t capacity = UINT64_C(1) << 32;
constexpr uint32_t fatFirst = 65536, fatSize = 524288, rootFirst = 2048, rootSize = 16384;
constexpr std::array<uint8_t, 16> basicType = {0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
                                               0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7};
constexpr std::array<uint8_t, 16> efiType = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                             0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
constexpr std::array<uint8_t, 16> apfsType = {0xef, 0x57, 0x34, 0x7c, 0x00, 0x00, 0xaa, 0x11,
                                              0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac};

void put(uint8_t *p, uint64_t n, unsigned bytes = 4)
{
    for (unsigned i = 0; i < bytes; ++i)
        p[i] = static_cast<uint8_t>(n >> (8 * i));
}

void mbrEntry(uint8_t *p, uint8_t type, uint32_t first, uint32_t length)
{
    p[4] = type;
    put(p + 8, first);
    put(p + 12, length);
}

void writeFat32Vbr(Sector &sector, uint32_t length)
{
    sector.fill(0);
    auto *b = sector.data();
    const uint32_t fat = (length / 4 + 129) / 128;
    put(b + 11, 512, 2);
    b[13] = 4;
    put(b + 14, 32, 2);
    b[16] = 2;
    put(b + 32, length);
    put(b + 36, fat > 1024 ? fat : 1024);
    put(b + 44, 2);
    put(b + 510, 0xaa55, 2);
}

void writeExfatVbr(Sector &sector, uint32_t length, unsigned shift = 9)
{
    sector.fill(0);
    auto *b = sector.data();
    memcpy(b + 3, "EXFAT   ", 8);
    const uint32_t logical = length >> (shift - 9);
    const uint32_t entriesPerSector = 1u << (shift - 2);
    const uint32_t minFat = (logical / 4 + 2 + entriesPerSector - 1) / entriesPerSector;
    const uint32_t fat = minFat > 1024 ? minFat : 1024;
    const uint32_t heap = 24 + fat > 2048 ? 24 + fat : 2048;
    put(b + 72, logical, 8);
    put(b + 80, 24);
    put(b + 84, fat);
    put(b + 88, heap);
    put(b + 92, (logical - heap) / 4);
    put(b + 96, 2);
    put(b + 104, 0x100, 2);
    b[108] = static_cast<uint8_t>(shift);
    b[109] = 2;
    b[110] = 1;
    put(b + 510, 0xaa55, 2);
}

class Card {
public:
    std::array<uint8_t, SWITCH_SD_REFERENCE_BYTES> page{};
    std::map<uint64_t, Sector> sectors;
    switch_gpt_workspace work{};
    switch_sd_files_slice slice{};
    uint64_t cardCapacity = capacity;
    uint32_t entryCount = 2;
    uint32_t reads = 0;

    Card()
    {
        auto &mbr = sectors[0];
        mbrEntry(mbr.data() + 446, 0x0c, fatFirst, fatSize);
        mbrEntry(mbr.data() + 462, 0xee, 1, UINT32_MAX);
        put(mbr.data() + 510, 0xaa55, 2);
        setEntry(0, basicType, 2, fatFirst, fatSize, "hos_data");
        setEntry(1, apfsType, 3, rootFirst, rootSize, "iOS");
        syncTables();

        constexpr std::array<uint32_t, 10> header = {
            SWITCH_SD_REFERENCE_MAGIC, 2, 0x500, 512, 512, 0x700b0000, 0, 1, 1, 0};
        for (unsigned i = 0; i < header.size(); ++i)
            put(page.data() + 4 * i, header[i]);
        put(page.data() + 0x2c, 1);
        auto *r = page.data() + SWITCH_SD_CARD_OFFSET;
        constexpr std::array<uint32_t, 8> fixed = {SWITCH_SD_CARD_MAGIC, 1, 256, 1, 0x700b0000, 1, 512, 1};
        for (unsigned i = 0; i < fixed.size(); ++i)
            put(r + 4 * i, fixed[i]);
        put(r + 0x20, cardCapacity, 8);
        put(r + 0x28, cardCapacity * 512, 8);
        put(r + 0x30, 0xc0000000);
        put(r + 0x34, 1);
        put(r + 0x38, 0x80000000);
        put(r + 0x40, 1u << 30);
        put(r + 0x44, (9u << 16) | 0x3f);
        put(r + 0x48, 0xffff0000);
        put(r + 0x50, 0x1b534d31);
        put(r + 0x60, 0xf0100000, 8);
        put(r + 0x68, 0xf0100100, 8);
        put(r + 0x70, 0xf0100200, 8);
        constexpr std::array<uint8_t, 32> firmware = {0xb4, 0x92, 0xa4, 0x01, 0x75, 0x80, 0xca, 0x11, 0xc9, 0xcd, 0x49,
                                                      0x07, 0x5a, 0x89, 0x9a, 0x65, 0xf6, 0x20, 0x69, 0xd3, 0x75, 0x06,
                                                      0x15, 0x21, 0x7f, 0xc1, 0x03, 0x08, 0x95, 0x4c, 0x9f, 0xaa};
        memcpy(r + 0x80, firmware.data(), firmware.size());
        put(r + 0xf0, switch_sd_crc32(r, 0xf0));
        handoffMbr();
        writeFat32Vbr(sectors[fatFirst], fatSize);
    }

    void setCapacity(uint64_t cap)
    {
        cardCapacity = cap;
        const uint32_t csize = static_cast<uint32_t>(cap / 1024 - 1);
        auto *r = page.data() + SWITCH_SD_CARD_OFFSET;
        put(r + 0x20, cardCapacity, 8);
        put(r + 0x28, cardCapacity * 512, 8);
        put(r + 0x44, (9u << 16) | (csize >> 16));
        put(r + 0x48, (csize & 0xffffu) << 16);
        put(r + 0xf0, switch_sd_crc32(r, 0xf0));
    }

    void clearEntries(uint32_t count)
    {
        entryCount = count;
        const uint32_t entrySectors = (entryCount * 128 + 511) / 512;
        for (uint32_t s = 0; s < entrySectors; ++s)
            sectors[2 + s].fill(0);
    }

    void setEntry(uint32_t slot, const std::array<uint8_t, 16> &type, uint8_t guidTag, uint64_t first, uint64_t length,
                  const char *name, uint64_t attrs = 0)
    {
        if (slot >= entryCount)
            entryCount = slot + 1;
        uint8_t *e = sectors[2 + slot / 4].data() + (slot % 4) * 128;
        memset(e, 0, 128);
        memcpy(e, type.data(), 16);
        e[16] = guidTag;
        put(e + 32, first, 8);
        put(e + 40, first + length - 1, 8);
        put(e + 48, attrs, 8);
        for (unsigned i = 0; name[i] && i < 36; ++i)
            e[56 + i * 2] = static_cast<uint8_t>(name[i]);
    }

    void setProtectiveMbr(uint32_t count)
    {
        sectors[0].fill(0);
        mbrEntry(sectors[0].data() + 446, 0xee, 1, count);
        put(sectors[0].data() + 510, 0xaa55, 2);
        handoffMbr();
    }

    void syncTables()
    {
        const uint32_t bytes = entryCount * 128;
        const uint32_t entrySectors = (bytes + 511) / 512;
        std::array<uint8_t, 128 * 128> raw{};
        for (uint32_t s = 0; s < entrySectors; ++s) {
            const auto &sec = sectors[2 + s];
            sectors[cardCapacity - 33 + s] = sec;
            memcpy(raw.data() + s * 512, sec.data(), 512);
        }
        const uint32_t arrayCrc = switch_sd_crc32(raw.data(), bytes);
        for (bool backup : {false, true}) {
            auto &header = sectors[backup ? cardCapacity - 1 : 1];
            header.fill(0);
            auto *h = header.data();
            memcpy(h, "EFI PART", 8);
            put(h + 8, 0x10000);
            put(h + 12, 92);
            put(h + 24, backup ? cardCapacity - 1 : 1, 8);
            put(h + 32, backup ? 1 : cardCapacity - 1, 8);
            put(h + 40, 34, 8);
            put(h + 48, cardCapacity - 34, 8);
            h[56] = 1;
            put(h + 72, backup ? cardCapacity - 33 : 2, 8);
            put(h + 80, entryCount);
            put(h + 84, 128);
            put(h + 88, arrayCrc);
            put(h + 16, switch_sd_crc32(h, 92));
        }
    }

    void handoffMbr()
    {
        auto &mbr = sectors[0];
        memcpy(page.data() + 512, mbr.data(), 512);
        uint32_t c = switch_sd_crc32(mbr.data(), 512);
        for (unsigned i = 0; i < 4; ++i)
            page[0x28 + i] = static_cast<uint8_t>(c >> (24 - 8 * i));
    }

    void exfat(uint8_t type, unsigned shift = 9)
    {
        sectors[0][450] = type;
        handoffMbr();
        writeExfatVbr(sectors[fatFirst], fatSize, shift);
    }

    static int read(void *context, uint64_t lba, uint8_t out[512])
    {
        auto &card = *static_cast<Card *>(context);
        ++card.reads;
        auto it = card.sectors.find(lba);
        if (it == card.sectors.end())
            return -1;
        memcpy(out, it->second.data(), 512);
        return 0;
    }

    int select(uint64_t root = rootFirst)
    {
        return switch_sd_files_select(read, this, page.data(), root, rootSize, &work, &slice);
    }
};
}  // namespace

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
