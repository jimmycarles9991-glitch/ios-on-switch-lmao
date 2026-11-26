// SPDX-License-Identifier: BSD-2-Clause-Patent
// An SD card for switch_sd_files_select: its sectors by LBA, the boot loader's reference page for it (the same-boot
// copy of sector 0 and the card's descriptor), and FAT32/exFAT boot sectors.
#pragma once
#include <array>
#include <cstring>
#include <map>
#include "../SwitchSdVolumeInternal.h"

namespace switch_sd_test {
using Sector = std::array<uint8_t, 512>;
constexpr uint64_t capacity = UINT64_C(1) << 32;
constexpr uint32_t fatFirst = 65536, fatSize = 524288, rootFirst = 2048, rootSize = 16384;
constexpr std::array<uint8_t, 16> basicType = {0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
                                               0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7};
constexpr std::array<uint8_t, 16> efiType = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                             0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
constexpr std::array<uint8_t, 16> apfsType = {0xef, 0x57, 0x34, 0x7c, 0x00, 0x00, 0xaa, 0x11,
                                              0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac};

inline void put(uint8_t *p, uint64_t n, unsigned bytes = 4)
{
    for (unsigned i = 0; i < bytes; ++i)
        p[i] = static_cast<uint8_t>(n >> (8 * i));
}

inline void mbrEntry(uint8_t *p, uint8_t type, uint32_t first, uint32_t length)
{
    p[4] = type;
    put(p + 8, first);
    put(p + 12, length);
}

inline void writeFat32Vbr(Sector &sector, uint32_t length, uint8_t cluster = 4)
{
    sector.fill(0);
    auto *b = sector.data();
    const uint32_t fat = (length / cluster + 129) / 128;
    put(b + 11, 512, 2);
    b[13] = cluster;
    put(b + 14, 32, 2);
    b[16] = 2;
    put(b + 32, length);
    put(b + 36, fat > 1024 ? fat : 1024);
    put(b + 44, 2);
    put(b + 510, 0xaa55, 2);
}

inline void writeExfatVbr(Sector &sector, uint32_t length, unsigned shift = 9, unsigned clusterShift = 2)
{
    sector.fill(0);
    auto *b = sector.data();
    memcpy(b + 3, "EXFAT   ", 8);
    const uint32_t logical = length >> (shift - 9);
    const uint32_t entriesPerSector = 1u << (shift - 2);
    const uint32_t minFat = ((logical >> clusterShift) + 2 + entriesPerSector - 1) / entriesPerSector;
    const uint32_t fat = minFat > 1024 ? minFat : 1024;
    const uint32_t heap = 24 + fat > 2048 ? 24 + fat : 2048;
    put(b + 72, logical, 8);
    put(b + 80, 24);
    put(b + 84, fat);
    put(b + 88, heap);
    put(b + 92, (logical - heap) >> clusterShift);
    put(b + 96, 2);
    put(b + 104, 0x100, 2);
    b[108] = static_cast<uint8_t>(shift);
    b[109] = static_cast<uint8_t>(clusterShift);
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
}  // namespace switch_sd_test
