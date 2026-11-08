// SPDX-License-Identifier: BSD-2-Clause-Patent
// r17-launch: the pure parts of SwitchCryptex.cpp's direct read map and boot prefetch list, kept free of kernel types
// so the host test (test/cryptex_map_test.cpp) checks this exact code.
//  - extents: the image's bytes as (image offset, SD window offset, length), sorted, contiguous from offset 0;
//  - pieces: a read of the image split into SD reads that each stay inside one extent and one SD command;
//  - runs: logged page ranges of the cryptex files merged into the prefetch's page-in runs;
//  - the list file's header (format SWCHPF02, frozen) and its check.
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

// A logged read of a cryptex file as pages [first, end), and a page-in run of the list.
struct SwitchRange {
    uint64_t fileId;
    uint32_t first, end;
    uint16_t when;  // 1 + first-read second
    uint8_t index;  // 0 System, 1 App
};

struct SwitchRun {
    uint64_t fileId;
    uint32_t page;
    uint16_t pages;
    uint8_t when;  // 1 + first-read second, at most 255
    uint8_t index;
};

// A run as the page-in UBC accepts: whole VM pages (cluster_pagein refuses anything else). The list counts listPage
// bytes per page, its frozen unit, so under larger VM pages a run widens to the VM pages it touches; it ends at the
// file's last VM page. False when the run starts at or past the end of the file.
struct SwitchPageIn {
    uint64_t offset, length;
};

inline bool switchRunPageIn(const SwitchRun &run, uint64_t listPage, uint64_t vmPage, uint64_t fileBytes,
                            SwitchPageIn *out)
{
    const uint64_t first = static_cast<uint64_t>(run.page) * listPage;
    const uint64_t mask = vmPage - 1;
    if (first >= fileBytes)
        return false;
    const uint64_t eof = (fileBytes + mask) & ~mask;
    const uint64_t end = (first + static_cast<uint64_t>(run.pages) * listPage + mask) & ~mask;
    out->offset = first & ~mask;
    out->length = (end < eof ? end : eof) - out->offset;
    return true;
}

// Enough runs for any merge of n ranges holding `pages` pages in all.
inline uint64_t switchRunCapacity(uint32_t n, uint64_t pages, uint32_t maxRun, uint32_t maxGap)
{
    return n + (pages + static_cast<uint64_t>(n) * maxGap) / maxRun + 1;
}

// Ranges sorted by (index, fileId, first) to runs: each file's ranges joined through gaps of at most maxGap pages, cut
// into runs of at most maxRun pages, each with the earliest second of what it joined. Returns the runs written; *files
// counts the distinct files.
inline uint32_t switchMergeRuns(const SwitchRange *ranges, uint32_t n, SwitchRun *runs, uint64_t capacity,
                                uint32_t maxRun, uint32_t maxGap, uint32_t *files)
{
    uint32_t count = 0;
    *files = 0;
    for (uint32_t i = 0; i < n;) {
        const SwitchRange &r = ranges[i];
        uint32_t end = r.end;
        uint16_t when = r.when;
        if (!i || r.fileId != ranges[i - 1].fileId || r.index != ranges[i - 1].index)
            ++*files;
        for (++i;
             i < n && ranges[i].fileId == r.fileId && ranges[i].index == r.index && ranges[i].first <= end + maxGap;
             ++i) {
            if (ranges[i].end > end)
                end = ranges[i].end;
            if (ranges[i].when < when)
                when = ranges[i].when;
        }
        for (uint32_t page = r.first; page < end && count < capacity; page += maxRun) {
            const uint32_t pages = end - page < maxRun ? end - page : maxRun;
            runs[count++] = {r.fileId, page, static_cast<uint16_t>(pages),
                             static_cast<uint8_t>(when < 255 ? when : 255), r.index};
        }
    }
    return count;
}

// The list's replay order: first-read second, then cryptex, file and page.
inline int switchCompareRuns(const SwitchRun &x, const SwitchRun &y)
{
    if (x.when != y.when)
        return x.when < y.when ? -1 : 1;
    if (x.index != y.index)
        return x.index < y.index ? -1 : 1;
    if (x.fileId != y.fileId)
        return x.fileId < y.fileId ? -1 : 1;
    return x.page < y.page ? -1 : static_cast<int>(x.page > y.page);
}

inline int switchCompareRanges(const SwitchRange &x, const SwitchRange &y)
{
    if (x.index != y.index)
        return x.index < y.index ? -1 : 1;
    if (x.fileId != y.fileId)
        return x.fileId < y.fileId ? -1 : 1;
    return x.first < y.first ? -1 : static_cast<int>(x.first > y.first);
}

// The prefetch list file: this header, then `runs` SwitchRun. This layout is format SWCHPF02, frozen by
// cryptex_map_test.cpp: a build whose loader refuses a list an earlier build saved makes its first boot a recording
// boot with no prefetch. So a new format keeps reading every older one and converts it (the older runs are still the
// boot's reads), and the test keeps a saved SWCHPF02 list loading.
constexpr uint32_t kSwitchListImages = 2;  // System, App (SWITCH_CRYPTEX_COUNT)

struct SwitchImageKey {
    uint64_t bytes, fileId;
    int64_t mtimeSec, mtimeNsec;
};

struct SwitchListHeader {
    char magic[8];
    uint32_t version, headerBytes;
    SwitchImageKey images[kSwitchListImages];
    uint32_t runs, files;
    uint64_t checksum;  // FNV-1a 64 of the runs
};

static_assert(sizeof(SwitchListHeader) == 96 && sizeof(SwitchRun) == 16, "the list format SWCHPF02");
constexpr char kSwitchListMagic[8] = {'S', 'W', 'C', 'H', 'P', 'F', '0', '2'};
constexpr uint32_t kSwitchListVersion = 2;

inline uint64_t switchFnv(const uint8_t *p, uint64_t n)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint64_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 0x100000001b3ULL;
    return h;
}

// The runs a list keeps, in replay order, until they hold `capPages` pages; returns how many.
inline uint32_t switchCapRuns(const SwitchRun *runs, uint32_t count, uint64_t capPages)
{
    uint64_t pages = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (pages + runs[i].pages > capPages)
            return i;
        pages += runs[i].pages;
    }
    return count;
}

// What a replay boot that reached SpringBoard says about its list. The foreground's reads of the cryptexes while the
// boot's window was open are the boot's reads the list lacks, or reached too late; hence the margin before they count.
struct SwitchReplayResult {
    uint64_t listPages, foregroundBytes;
    uint32_t missingFiles;
};

constexpr uint64_t kSwitchStaleForegroundBytes = 64ULL << 20;

// Why the next boot records afresh, or nullptr to keep replaying the list.
inline const char *switchListStale(const SwitchReplayResult &r, uint64_t capPages, uint64_t pageBytes)
{
    if (r.missingFiles)
        return "files it names are gone";
    if (r.listPages > capPages)
        return "it holds more than a boot reads";
    if (r.foregroundBytes > kSwitchStaleForegroundBytes && r.foregroundBytes > r.listPages * pageBytes / 2)
        return "the boot read half as much again outside it";
    return nullptr;
}

// Whether a saved header is a list this build replays for these images (at most maxRuns runs), or why not.
inline const char *switchListRefusal(const SwitchListHeader &saved, const SwitchImageKey (&images)[2], uint32_t maxRuns)
{
    for (unsigned i = 0; i < sizeof(kSwitchListMagic); ++i) {
        if (saved.magic[i] != kSwitchListMagic[i])
            return "not an SWCHPF02 list";
    }
    if (saved.version != kSwitchListVersion || saved.headerBytes != sizeof(SwitchListHeader))
        return "an SWCHPF02 list of another version";
    for (unsigned i = 0; i < kSwitchListImages; ++i) {
        if (saved.images[i].bytes != images[i].bytes || saved.images[i].fileId != images[i].fileId ||
            saved.images[i].mtimeSec != images[i].mtimeSec || saved.images[i].mtimeNsec != images[i].mtimeNsec)
            return "saved for other cryptex images";
    }
    if (!saved.runs || saved.runs > maxRuns)
        return "no runs, or too many";
    return nullptr;
}
#endif
