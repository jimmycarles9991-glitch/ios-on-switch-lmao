// SPDX-License-Identifier: BSD-2-Clause-Patent
// What setup() takes from the card's partition tables (NativeSwitchBlock.cpp reads the sectors): the iOS root from the
// primary GPT, and the word its boot log gives when the shared FAT volume (Files) stays offline.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "SwitchSdWindow.h"

extern "C" {
#include "../../diagnostics/switch/storage/partition.h"
}

constexpr uint32_t kSwitchSdGptArrayBytes = 64 * 1024;

struct SwitchSdGptArray {
    uint64_t lba = 0;
    uint32_t count = 0, size = 0;
};

inline uint32_t switchSdGptLe32(const uint8_t *p)
{
    return p[0] | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

inline uint64_t switchSdGptLe64(const uint8_t *p)
{
    return switchSdGptLe32(p) | static_cast<uint64_t>(switchSdGptLe32(p + 4)) << 32;
}

// The primary header (LBA 1)'s entry array; false for a header the driver does not read. Neither CRC nor the backup
// copy is checked.
inline bool switchSdGptArray(const uint8_t header[512], SwitchSdGptArray *array)
{
    array->lba = switchSdGptLe64(header + 72);
    array->count = switchSdGptLe32(header + 80);
    array->size = switchSdGptLe32(header + 84);
    return !(memcmp(header, "EFI PART", 8) || array->size < 128 || array->size > 512 || (array->size & 7) ||
             !array->count || array->count > kSwitchSdGptArrayBytes / array->size || array->lba < 2 ||
             array->lba > UINT32_MAX);
}

// The sectors holding a read header's count * size bytes of entries; 0 when they would run past LBA 2^32 - 1.
inline uint32_t switchSdGptArraySectors(const SwitchSdGptArray &array)
{
    const uint32_t sectors = (array.count * array.size + 511) / 512;
    return sectors - 1 > UINT32_MAX - array.lba ? 0 : sectors;
}

// The entry's name field (36 UTF-16LE code units) holds exactly the ASCII `name`.
inline bool switchSdGptNamed(const uint8_t *entry, const char *name)
{
    const uint8_t *field = entry + 56;
    unsigned i = 0;
    for (; name[i]; ++i) {
        if (i == 36 || field[2 * static_cast<size_t>(i)] != static_cast<uint8_t>(name[i]) || field[2 * i + 1])
            return false;
    }
    return i == 36 || ((field[2 * static_cast<size_t>(i)] == 0u) && (field[2 * i + 1] == 0u));
}

struct SwitchSdRoot {
    SwitchSdWindow window;  // the last match's
    unsigned found = 0;

    bool unique() const
    {
        return found == 1 && window.contains(window.first, window.count);
    }
};

// Every declared entry typed `type` or named `name`, in any slot, whose extent one card command can address (LBA
// below 2^32). `entries` holds the array's count * size bytes (switchSdGptArray bounds them to 64 KiB).
inline SwitchSdRoot switchSdFindRoot(const uint8_t *entries, const SwitchSdGptArray &array, const uint8_t type[16],
                                     const char *name)
{
    SwitchSdRoot root;
    for (uint32_t i = 0; i < array.count; ++i) {
        const uint8_t *e = entries + static_cast<size_t>(i) * array.size;
        if (memcmp(e, type, 16) && !switchSdGptNamed(e, name))
            continue;
        const uint64_t first = switchSdGptLe64(e + 32);
        const uint64_t last = switchSdGptLe64(e + 40);
        if (!first || last < first || last > UINT32_MAX)
            continue;
        root.window.first = first;
        root.window.count = last - first + 1;
        ++root.found;
    }
    return root;
}

// setup()'s status for a shared volume that switch_sd_files_select found on a card other than the one booted.
constexpr int kSwitchSdFilesOtherCard = -1;

// The shared volume's status from the lookup's: without the booted card's CID, the volume's card is taken as it.
inline int switchSdFilesStatus(int lookup, bool haveCid, const uint8_t bootCid[16], const uint8_t volumeCid[16])
{
    return lookup == SWITCH_GPT_OK && haveCid && memcmp(bootCid, volumeCid, 15) ? kSwitchSdFilesOtherCard : lookup;
}

inline const char *switchSdFilesReason(int status)
{
    switch (status) {
    case kSwitchSdFilesOtherCard:
        return "other-card";
    case SWITCH_GPT_ARGUMENT:
        return "argument";
    case SWITCH_GPT_IO:
        return "io";
    case SWITCH_GPT_MBR:
        return "mbr";
    case SWITCH_GPT_HEADER:
        return "header";
    case SWITCH_GPT_HEADER_CRC:
        return "header-crc";
    case SWITCH_GPT_GEOMETRY:
        return "geometry";
    case SWITCH_GPT_ENTRIES:
        return "entries";
    case SWITCH_GPT_ARRAY_CRC:
        return "array-crc";
    case SWITCH_GPT_MISMATCH:
        return "mismatch";
    case SWITCH_GPT_NOT_FOUND:
        return "not-found";
    case SWITCH_GPT_OVERLAP:
        return "overlap";
    case SWITCH_GPT_DUPLICATE:
        return "duplicate";
    case SWITCH_GPT_PROBE:
        return "probe";
    case SWITCH_GPT_AMBIGUOUS:
        return "ambiguous";
    default:
        return "unknown";
    }
}
