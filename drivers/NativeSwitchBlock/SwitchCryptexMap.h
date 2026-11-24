// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pure extent/piece helpers for SwitchCryptex.cpp's direct read map, kept free of kernel types so host tests
// (test/cryptex_map_test.cpp) exercise the exact code.
#ifndef SWITCH_CRYPTEX_MAP_H
#define SWITCH_CRYPTEX_MAP_H
#include <stdint.h>

struct SwitchExtent {
    uint64_t file, disk,
        length;  // bytes; disk is kSwitchHole for a range the file does not store (it reads as zeros)
};

constexpr uint64_t kSwitchHole = ~0ULL;

// Appends an extent, merged into the last one when it continues it in the image and on the card (or both are holes).
// False when full.
inline bool switchAppendExtent(SwitchExtent *map, uint32_t *count, uint32_t capacity, uint64_t file, uint64_t disk,
                               uint64_t length)
{
    const SwitchExtent *last = *count ? &map[*count - 1] : nullptr;
    if (last && last->file + last->length == file &&
        (last->disk == kSwitchHole ? disk == kSwitchHole : disk != kSwitchHole && last->disk + last->length == disk)) {
        map[*count - 1].length += length;
        return true;
    }
    if (*count == capacity)
        return false;
    map[(*count)++] = {file, disk, length};
    return true;
}

// Whether the extents cover [0, bytes) of the image in order, with nothing missing or overlapping.
inline bool switchExtentsCover(const SwitchExtent *map, uint32_t count, uint64_t bytes)
{
    uint64_t at = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (map[i].file != at || !map[i].length)
            return false;
        at += map[i].length;
    }
    return (count != 0u) && at == bytes;
}

// The extent holding image offset `offset` (the last one that begins at or before it).
inline uint32_t switchExtentAt(const SwitchExtent *map, uint32_t count, uint64_t offset)
{
    uint32_t lo = 0;
    uint32_t hi = count;
    while (hi - lo > 1) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (map[mid].file <= offset) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// The next SD read of an image read now at `offset` with `remaining` bytes left: inside extent *at, at most maxPiece
// bytes (disk kSwitchHole: zeros, no read). Advances *at past the extent when the piece ends it. length 0 when the map
// has nothing there.
struct SwitchPiece {
    uint64_t disk, length;
};

inline SwitchPiece switchNextPiece(const SwitchExtent *map, uint32_t count, uint32_t *at, uint64_t offset,
                                   uint64_t remaining, uint64_t maxPiece)
{
    if (*at >= count || offset < map[*at].file || offset - map[*at].file >= map[*at].length)
        return {0, 0};
    const SwitchExtent &e = map[*at];
    const uint64_t within = offset - e.file;
    uint64_t length = e.length - within;
    if (length > remaining)
        length = remaining;
    if (length > maxPiece)
        length = maxPiece;
    if (within + length == e.length)
        ++*at;
    return {e.disk == kSwitchHole ? kSwitchHole : e.disk + within, length};
}
#endif
