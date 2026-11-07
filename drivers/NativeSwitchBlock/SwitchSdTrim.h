// SPDX-License-Identifier: BSD-2-Clause-Patent
// Erase-based trim for an SD card without DISCARD_SUPPORT: the allocation units APFS unmapped, kept as one bit each
// and erased later, one run per command, in idle time (NativeSwitchBlock.cpp). An AU is erased only whole (a partial
// erase makes the card copy the rest of the AU) and only if nothing wrote to it since APFS freed it: a write clears the
// bits of every AU it touches when it is queued, before it can reach the card, so a deferred erase never destroys data.
// AU k covers card LBAs [k * auSectors, (k + 1) * auSectors); the map holds the whole AUs of one partition.
#ifndef SWITCH_SD_TRIM_H
#define SWITCH_SD_TRIM_H

#include <stddef.h>
#include <stdint.h>

struct SwitchSdTrimMap {
    uint64_t *bits = nullptr;
    uint64_t firstAu = 0;  // the partition's first whole AU
    uint64_t aus = 0;      // whole AUs in the partition
    uint32_t auSectors = 0;
    uint64_t cursor = 0;   // the AU (map index) the next search starts at
    uint64_t pending = 0;  // bits set
};

// The 64-bit words a map of the partition [first, first + sectors) needs; 0 when it holds no whole AU.
inline uint64_t switchSdTrimWords(uint64_t first, uint64_t sectors, uint32_t auSectors)
{
    if (!auSectors)
        return 0;
    const uint64_t firstAu = (first + auSectors - 1) / auSectors, endAu = (first + sectors) / auSectors;
    return endAu > firstAu ? (endAu - firstAu + 63) / 64 : 0;
}

// `storage` holds switchSdTrimWords() zeroed words.
inline void switchSdTrimInit(SwitchSdTrimMap *m, uint64_t *storage, uint64_t first, uint64_t sectors,
                             uint32_t auSectors)
{
    m->bits = storage;
    m->auSectors = auSectors;
    m->firstAu = (first + auSectors - 1) / auSectors;
    const uint64_t endAu = (first + sectors) / auSectors;
    m->aus = endAu > m->firstAu ? endAu - m->firstAu : 0;
    m->cursor = 0;
    m->pending = 0;
}

inline bool switchSdTrimTest(const SwitchSdTrimMap *m, uint64_t au)
{
    return m->bits[au / 64] >> (au % 64) & 1;
}

inline void switchSdTrimSet(SwitchSdTrimMap *m, uint64_t au, bool on)
{
    const uint64_t bit = 1ull << (au % 64);
    if (on == !!(m->bits[au / 64] & bit))
        return;
    m->bits[au / 64] ^= bit;
    m->pending += on ? 1 : -1;
}

// APFS unmapped card LBAs [lba, lba + count): the AUs wholly inside become due.
inline void switchSdTrimMark(SwitchSdTrimMap *m, uint64_t lba, uint64_t count)
{
    if (!m->aus)
        return;
    uint64_t au = (lba + m->auSectors - 1) / m->auSectors, end = (lba + count) / m->auSectors;
    au = au > m->firstAu ? au - m->firstAu : 0;
    end = end > m->firstAu ? end - m->firstAu : 0;
    for (end = end < m->aus ? end : m->aus; au < end; ++au)
        switchSdTrimSet(m, au, true);
}

// A write of [lba, lba + count) is queued: no AU it touches may be erased any more.
inline void switchSdTrimClear(SwitchSdTrimMap *m, uint64_t lba, uint64_t count)
{
    if (!m->pending || !count)
        return;
    uint64_t au = lba / m->auSectors, end = (lba + count - 1) / m->auSectors + 1;
    au = au > m->firstAu ? au - m->firstAu : 0;
    end = end > m->firstAu ? end - m->firstAu : 0;
    for (end = end < m->aus ? end : m->aus; au < end; ++au)
        switchSdTrimSet(m, au, false);
}

// The next due run of at most maxAus AUs, from the cursor on (wrapping once): its card LBA and length in AUs, its bits
// cleared and the cursor past it. False when nothing is due.
inline bool switchSdTrimNext(SwitchSdTrimMap *m, uint32_t maxAus, uint64_t *lba, uint32_t *aus)
{
    if (!m->pending || !maxAus)
        return false;
    if (m->cursor >= m->aus)
        m->cursor = 0;
    uint64_t au = m->cursor;
    for (uint64_t scanned = 0; scanned < m->aus; ++scanned, au = au + 1 < m->aus ? au + 1 : 0) {
        if (!(au % 64) && !m->bits[au / 64] && m->aus - au >= 64) {
            scanned += 63;
            au += 63;
            continue;
        }
        if (!switchSdTrimTest(m, au))
            continue;
        uint32_t n = 0;
        while (n < maxAus && au + n < m->aus && switchSdTrimTest(m, au + n))
            switchSdTrimSet(m, au + n++, false);
        *lba = (m->firstAu + au) * m->auSectors;
        *aus = n;
        m->cursor = au + n < m->aus ? au + n : 0;
        return true;
    }
    return false;
}

#endif
