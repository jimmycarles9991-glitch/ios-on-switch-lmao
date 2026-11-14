// SPDX-License-Identifier: BSD-2-Clause-Patent
// Whether window A's direct apertures reach the assembly registers the WINDOW_HEADER-selected copy does, for the four
// words window A's completion flip writes (SwitchCompletionFlipDisplay.h): DC_WIN_AD at 0xb80 (TRM 24.10.10 LINE_STRIDE
// at 0xb8a) and DC_WINBUF_AD at 0xbc0 (24.11.1 START_ADDR, 24.11.4 ADDR_H_OFFSET at 0xbc6, 24.11.7 SURFACE_KIND at
// 0xbcb), against DC_WIN 0x700 + i and DC_WINBUF 0x800 + i with window A selected. Each word is written one way and read
// the other, both ways, then put back; a bit flipped in each word's own field (START_ADDR's address, the offset's low
// bytes, BLOCK_HEIGHT, LINE_STRIDE), so a reserved bit never reads back differently. Assembly copies only: the caller
// holds every writer of window A off, so no UPDATE promotes a test value and DC A never scans one. Plain logic over a
// register accessor (display.completion_flip winbuf_alias).
#pragma once
#include <stdint.h>

namespace switch_window_aperture {

constexpr uint32_t kStateAccess = 0x040, kWindowHeader = 0x042, kWindowASelect = 1u << 4;
constexpr uint32_t kMuxActive = (1u << 0) | (1u << 2);  // READ_MUX and WRITE_MUX ACTIVE

struct Word {
    uint32_t header, aperture, first, second;  // the two bits flipped, one for each way
};

constexpr Word kWords[] = {
    {0x800, 0xbc0, 1u << 20, 1u << 21},  // START_ADDR
    {0x806, 0xbc6, 1u << 6, 1u << 7},    // ADDR_H_OFFSET
    {0x80b, 0xbcb, 1u << 4, 1u << 5},    // SURFACE_KIND's BLOCK_HEIGHT
    {0x70a, 0xb8a, 1u << 8, 1u << 9},    // LINE_STRIDE
};
constexpr unsigned kWordCount = sizeof(kWords) / sizeof(kWords[0]);
constexpr uint32_t kAllWords = (1u << kWordCount) - 1;

struct Result {
    uint32_t aliased;   // a bit per word: each path read back what the other wrote
    uint32_t restored;  // a bit per word: both paths read its value from before
};

template <class Registers> Result check(Registers &dc)
{
    const uint32_t access = dc[kStateAccess], header = dc[kWindowHeader];
    dc[kStateAccess] = access & ~kMuxActive;
    dc[kWindowHeader] = kWindowASelect;
    Result result = {};
    for (unsigned i = 0; i < kWordCount; ++i) {
        const Word &w = kWords[i];
        const uint32_t before = dc[w.header], beforeAperture = dc[w.aperture];
        dc[w.aperture] = before ^ w.first;
        const bool seenThroughHeader = dc[w.header] == (before ^ w.first);
        dc[w.header] = before ^ w.second;
        const bool seenThroughAperture = dc[w.aperture] == (before ^ w.second);
        dc[w.header] = before;
        dc[w.aperture] = beforeAperture;
        if (seenThroughHeader && seenThroughAperture)
            result.aliased |= 1u << i;
        if (dc[w.header] == before && dc[w.aperture] == beforeAperture)
            result.restored |= 1u << i;
    }
    dc[kWindowHeader] = header;
    dc[kStateAccess] = access;
    return result;
}

}  // namespace switch_window_aperture
