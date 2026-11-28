// SPDX-License-Identifier: BSD-2-Clause-Patent
// r12-sdio: which queued requests ride in one card command with the request the SD worker picked (NativeSwitchBlock.cpp
// pickLocked), wherever they sit in the queue.
//
// Until r12-sdio a chain only took the requests right behind its first one in the queue, so two reads that continue
// each other on the card but were queued with another request between them took two commands (~0.58 ms each on r7p5,
// most of it the card's access time; 4 KiB of data is ~40 us of it). A read chain grows at either end with any queued
// read (among the first `scan`) that continues it, and a write chain grows at either end with any queued write (among
// the first `scan`) that continues it strictly contiguously (no gaps), as long as:
// - it matches the chain's direction;
// - it overlaps no older request still queued (nor, for writes, any active in-flight range), preserving RAW/WAR/WAW
//   ordering;
// - the chain stays within one command (`limit` sectors, `maxCount` requests).
// The chain comes back in card order (rising LBA), so the descriptor table and the bounce path walk it as before.
//
// r14-sdcq: with `maxGap` and `gapBudget` (sectors) a queued read also joins when it starts up to `maxGap` past the chain's
// end (or ends up to `maxGap` before its start), or falls inside the chain's range without overlapping a member: the
// card reads straight through the gaps between members, into a sink the driver never looks at, so two nearby reads cost
// one command plus the gap's transfer instead of two commands. No single gap is longer than `maxGap` and all of one
// chain's gaps add up to at most `gapBudget`. With both 0 (the default) nothing changes: members tile the range.
// Writes never use gaps: payload always equals sectors.
#pragma once

#include <stdint.h>

template <typename Request> inline bool switchOverlaps(const Request *a, const Request *b)
{
    return a->lba < b->lba + b->sectors && b->lba < a->lba + a->sectors;
}

struct SwitchLbaRange {
    uint64_t lba;
    uint32_t sectors;
};

struct SwitchChainBounds {
    uint64_t start;
    uint32_t sectors;
    uint32_t count;
    uint32_t payload;
    uint32_t maxGapSeen;
    bool write;
    bool valid;
};

inline bool switchChainBoundsFit(const SwitchChainBounds &b, uint32_t maxGap, uint32_t gapBudget)
{
    if (!b.valid || !b.count || !b.sectors || b.payload > b.sectors)
        return false;
    if (b.write)
        return !maxGap && !gapBudget && b.payload == b.sectors && b.maxGapSeen == 0;
    return b.maxGapSeen <= maxGap && (b.sectors - b.payload) <= gapBudget;
}

inline uint64_t switchRangeBitMask(uint64_t lo, uint64_t hi, uint64_t base, uint64_t bucketSpan)
{
    if (hi <= base)
        return 0;
    const uint64_t s = lo > base ? (lo - base) / bucketSpan : 0;
    if (s >= 64)
        return 0;
    uint64_t e = (hi - 1 - base) / bucketSpan;
    if (e >= 64)
        e = 63;
    return (~0ULL >> (63 - (e - s))) << s;
}

template <typename Request> inline bool switchOverlapsRange(const Request *a, const SwitchLbaRange &b)
{
    return a->sectors != 0 && b.sectors != 0 && a->lba < b.lba + b.sectors && b.lba < a->lba + a->sectors;
}

template <typename Request>
inline auto switchSameWindow(const Request *a, const Request *b, int) -> decltype(a->shared == b->shared)
{
    return a->shared == b->shared;
}

template <typename Request> inline bool switchSameWindow(const Request *, const Request *, ...)
{
    return true;
}

// Marks `first` and the reads that join it (`chained`), returns how many, and the chain's first LBA and length (gaps
// included). `first` is in the queue; nothing is unlinked yet (switchUnlinkChain does that).
template <typename Request>
inline uint32_t switchGatherReads(Request *head, Request *first, uint32_t limit, uint32_t maxCount, unsigned scan,
                                  uint64_t *start, uint32_t *sectors, uint32_t maxGap = 0, uint32_t gapBudget = 0,
                                  SwitchChainBounds *bounds = nullptr)
{
    first->chained = true;
    uint64_t low = first->lba;
    uint64_t high = first->lba + first->sectors;
    uint64_t payload = first->sectors;
    uint32_t count = 1;
    uint32_t maxGapSeen = 0;
    const auto finish = [&](bool valid) {
        const uint32_t totalSectors = static_cast<uint32_t>(high - low);
        *start = low;
        *sectors = totalSectors;
        if (bounds)
            *bounds = {low, totalSectors, count, static_cast<uint32_t>(payload), maxGapSeen, false, valid};
        return count;
    };
    if (!head || !head->next || maxCount <= 1 || !scan)
        return finish(first->sectors != 0 && !first->write);
    const uint64_t minReach = low > limit ? low - limit : 0;
    const uint64_t maxReach = (~0ULL - high) > limit ? high + limit : ~0ULL;
    const uint64_t reachSpan = maxReach - minReach;
    const uint64_t bucketSpan = reachSpan > 64 ? (reachSpan + 63) / 64 : 1;

    constexpr unsigned kNearCap = 256;
    Request *near[kNearCap];
    uint64_t nearMask[kNearCap];
    unsigned nearCount = 0;
    unsigned candidates = 0;
    bool overflow = false;
    unsigned seen = 0;
    for (Request *r = head; r && seen < scan; r = r->next, ++seen) {
        if (r == first || r->chained)
            continue;
        if (r->lba >= maxReach || r->lba + r->sectors <= minReach)
            continue;
        if (nearCount == kNearCap) {
            overflow = true;
            break;
        }
        near[nearCount] = r;
        nearMask[nearCount] = switchRangeBitMask(r->lba, r->lba + r->sectors, minReach, bucketSpan);
        ++nearCount;
        if (!r->write && switchSameWindow(r, first, 0))
            ++candidates;
    }
    if (!overflow && !candidates)
        return finish(first->sectors != 0 && !first->write);

    constexpr unsigned kGapCap = 64;
    SwitchLbaRange gaps[kGapCap];
    unsigned gapCount = 0;
    bool gapsOverflow = false;
    const auto recordGap = [&](uint64_t at, uint32_t len) {
        if (!len)
            return;
        if (len > maxGapSeen)
            maxGapSeen = len;
        if (gapCount < kGapCap) {
            gaps[gapCount++] = {at, len};
        } else {
            gapsOverflow = true;
        }
    };

    if (!overflow) {
        for (bool grew = true; grew && count < maxCount;) {
            grew = false;
            uint64_t olderMask = 0;
            for (unsigned i = 0; i < nearCount && count < maxCount; ++i) {
                Request *const r = near[i];
                if (r->chained)
                    continue;
                const uint64_t rMask = nearMask[i];
                if (r->write || !switchSameWindow(r, first, 0)) {
                    olderMask |= rMask;
                    continue;
                }
                const uint64_t from = r->lba;
                const uint64_t to = r->lba + r->sectors;
                uint64_t newLow = low;
                uint64_t newHigh = high;
                uint64_t gap = 0;
                bool inside = false;
                if (from >= high) {
                    gap = from - high;
                    newHigh = to;
                } else if (to <= low) {
                    gap = low - to;
                    newLow = from;
                } else if (maxGap && from >= low && to <= high) {
                    inside = true;
                } else {
                    olderMask |= rMask;
                    continue;
                }
                if (gap > maxGap || newHigh - newLow > limit ||
                    (newHigh - newLow) - (payload + r->sectors) > gapBudget) {
                    olderMask |= rMask;
                    continue;
                }
                unsigned insideGapIdx = kGapCap;
                bool overlaps = false;
                if (inside) {
                    if (!gapsOverflow) {
                        for (unsigned g = 0; g < gapCount; ++g) {
                            if (from >= gaps[g].lba && to <= gaps[g].lba + gaps[g].sectors) {
                                insideGapIdx = g;
                                break;
                            }
                        }
                        if (insideGapIdx == kGapCap)
                            overlaps = true;
                    } else {
                        for (const Request *member = head; member && !overlaps; member = member->next)
                            overlaps = member->chained && switchOverlaps(member, r);
                    }
                }
                if (!overlaps && (olderMask & rMask) != 0) {
                    for (unsigned j = 0; j < i && !overlaps; ++j) {
                        if (!near[j]->chained && (nearMask[j] & rMask) != 0)
                            overlaps = switchOverlaps(near[j], r);
                    }
                }
                if (overlaps) {
                    olderMask |= rMask;
                    continue;
                }
                r->chained = true;
                ++count;
                payload += r->sectors;
                if (inside) {
                    if (!gapsOverflow && insideGapIdx < gapCount) {
                        const uint64_t gLo = gaps[insideGapIdx].lba;
                        const uint64_t gHi = gLo + gaps[insideGapIdx].sectors;
                        gaps[insideGapIdx] = gaps[--gapCount];
                        if (from > gLo)
                            recordGap(gLo, static_cast<uint32_t>(from - gLo));
                        if (to < gHi)
                            recordGap(to, static_cast<uint32_t>(gHi - to));
                    }
                } else if (from >= high) {
                    recordGap(high, static_cast<uint32_t>(gap));
                } else {
                    recordGap(to, static_cast<uint32_t>(gap));
                }
                low = newLow;
                high = newHigh;
                grew = true;
            }
        }
        return finish(first->sectors != 0 && !first->write);
    }

    for (bool grew = true; grew && count < maxCount;) {
        grew = false;
        unsigned seenPass = 0;
        for (Request *r = head; r && seenPass < scan && count < maxCount; r = r->next, ++seenPass) {
            if (r->write || r->chained || !switchSameWindow(r, first, 0))
                continue;
            const uint64_t from = r->lba;
            const uint64_t to = r->lba + r->sectors;
            uint64_t newLow = low;
            uint64_t newHigh = high;
            uint64_t gap = 0;
            bool inside = false;
            if (from >= high) {
                gap = from - high;
                newHigh = to;
            } else if (to <= low) {
                gap = low - to;
                newLow = from;
            } else if (maxGap && from >= low && to <= high) {
                inside = true;
            } else {
                continue;
            }
            if (gap > maxGap || newHigh - newLow > limit || (newHigh - newLow) - (payload + r->sectors) > gapBudget)
                continue;
            bool overlaps = false;
            for (const Request *older = head; older != r && !overlaps; older = older->next)
                overlaps = !older->chained && switchOverlaps(older, r);
            for (const Request *member = head; inside && member && !overlaps; member = member->next)
                overlaps = member->chained && switchOverlaps(member, r);
            if (overlaps)
                continue;
            r->chained = true;
            ++count;
            payload += r->sectors;
            if (gap > maxGapSeen)
                maxGapSeen = static_cast<uint32_t>(gap);
            low = newLow;
            high = newHigh;
            grew = true;
        }
    }
    return finish(first->sectors != 0 && !first->write);
}

// `inOrder`: a synchronize waits on this window's writes. Its barrier counts completions (SwitchSdWriteFence.h), so
// while it waits no write may join past an older write still queued: one submitted after the barrier would complete in
// the place of one it waits for. With no barrier waiting, any barrier that begins later counts both writes. This keeps
// the chain a prefix of the window's queued writes only because the pick starts a write chain with the window's oldest
// queued write; a pick that let `first` pass an older write would break the fence with or without this rule.
template <typename Request>
inline uint32_t switchGatherWrites(Request *head, Request *first, uint32_t limit, uint32_t maxCount, unsigned scan,
                                   uint64_t *start, uint32_t *sectors, const SwitchLbaRange *inFlight = nullptr,
                                   unsigned inFlightCount = 0, bool inOrder = false,
                                   SwitchChainBounds *bounds = nullptr)
{
    first->chained = true;
    uint64_t low = first->lba;
    uint64_t high = first->lba + first->sectors;
    uint32_t count = 1;
    const auto finish = [&](bool valid) {
        const uint32_t totalSectors = static_cast<uint32_t>(high - low);
        *start = low;
        *sectors = totalSectors;
        if (bounds)
            *bounds = {low, totalSectors, count, totalSectors, 0, true, valid};
        return count;
    };
    if (!head || !head->next || maxCount <= 1 || !scan)
        return finish(first->sectors != 0 && first->write);
    const uint64_t minReach = low > limit ? low - limit : 0;
    const uint64_t maxReach = (~0ULL - high) > limit ? high + limit : ~0ULL;
    const uint64_t reachSpan = maxReach - minReach;
    const uint64_t bucketSpan = reachSpan > 64 ? (reachSpan + 63) / 64 : 1;

    constexpr unsigned kNearCap = 256;
    Request *near[kNearCap];
    uint64_t nearMask[kNearCap];
    unsigned nearCount = 0;
    unsigned candidates = 0;
    bool overflow = false;
    unsigned seen = 0;
    for (Request *r = head; r && seen < scan; r = r->next, ++seen) {
        if (r == first || r->chained)
            continue;
        const bool sameWindowWrite = r->write && switchSameWindow(r, first, 0);
        const bool inReach = r->lba < maxReach && r->lba + r->sectors > minReach;
        if (!inReach && !(inOrder && sameWindowWrite))
            continue;
        if (nearCount == kNearCap) {
            overflow = true;
            break;
        }
        near[nearCount] = r;
        nearMask[nearCount] = inReach ? switchRangeBitMask(r->lba, r->lba + r->sectors, minReach, bucketSpan) : 0;
        ++nearCount;
        if (sameWindowWrite && inReach)
            ++candidates;
        // In inOrder mode, no same-window write after an out-of-reach same-window write can ever join.
        if (inOrder && sameWindowWrite && !inReach)
            break;
    }
    if (!overflow && !candidates)
        return finish(first->sectors != 0 && first->write);

    if (!overflow) {
        for (bool grew = true; grew && count < maxCount;) {
            grew = false;
            uint64_t olderMask = 0;
            for (unsigned i = 0; i < nearCount && count < maxCount; ++i) {
                Request *const r = near[i];
                if (r->chained)
                    continue;
                const uint64_t rMask = nearMask[i];
                const bool sameWindowWrite = r->write && switchSameWindow(r, first, 0);
                if (!sameWindowWrite) {
                    olderMask |= rMask;
                    continue;
                }
                const uint64_t from = r->lba;
                const uint64_t to = r->lba + r->sectors;
                uint64_t newLow = low;
                uint64_t newHigh = high;
                bool adjacent = false;
                if (from == high) {
                    newHigh = to;
                    adjacent = true;
                } else if (to == low) {
                    newLow = from;
                    adjacent = true;
                }
                if (!adjacent || newHigh - newLow > limit) {
                    if (inOrder)
                        break;
                    olderMask |= rMask;
                    continue;
                }
                bool overlaps = false;
                for (unsigned f = 0; f < inFlightCount && !overlaps; ++f)
                    overlaps = switchOverlapsRange(r, inFlight[f]);
                if (!overlaps && (olderMask & rMask) != 0) {
                    for (unsigned j = 0; j < i && !overlaps; ++j) {
                        if (!near[j]->chained && (nearMask[j] & rMask) != 0)
                            overlaps = switchOverlaps(near[j], r);
                    }
                }
                if (overlaps) {
                    if (inOrder)
                        break;
                    olderMask |= rMask;
                    continue;
                }
                r->chained = true;
                ++count;
                low = newLow;
                high = newHigh;
                grew = true;
            }
        }
        return finish(first->sectors != 0 && first->write);
    }

    for (bool grew = true; grew && count < maxCount;) {
        grew = false;
        unsigned seenPass = 0;
        for (Request *r = head; r && seenPass < scan && count < maxCount; r = r->next, ++seenPass) {
            if (!r->write || r->chained || !switchSameWindow(r, first, 0))
                continue;
            const uint64_t from = r->lba;
            const uint64_t to = r->lba + r->sectors;
            uint64_t newLow = low;
            uint64_t newHigh = high;
            if (from == high) {
                newHigh = to;
            } else if (to == low) {
                newLow = from;
            } else {
                continue;
            }
            if (newHigh - newLow > limit)
                continue;
            bool overlaps = false;
            for (unsigned i = 0; i < inFlightCount && !overlaps; ++i)
                overlaps = switchOverlapsRange(r, inFlight[i]);
            for (const Request *older = head; older != r && !overlaps; older = older->next) {
                overlaps = !older->chained &&
                           (switchOverlaps(older, r) || (inOrder && older->write && switchSameWindow(older, r, 0)));
            }
            if (overlaps)
                continue;
            r->chained = true;
            ++count;
            low = newLow;
            high = newHigh;
            grew = true;
        }
    }
    return finish(first->sectors != 0 && first->write);
}

template <typename Request>
inline bool switchChainFitsDirection(const Request *head, uint64_t start, uint32_t sectors, uint32_t count,
                                     uint32_t maxGap, uint32_t gapBudget, bool write)
{
    if (write && (maxGap || gapBudget))
        return false;
    constexpr uint32_t kStackCap = 256;
    if (count > kStackCap) {
        uint64_t total = 0;
        uint32_t found = 0;
        const Request *first = nullptr;
        bool atStart = false;
        bool atEnd = false;
        for (const Request *r = head; r; r = r->next) {
            if (!r->chained)
                continue;
            if (!first)
                first = r;
            ++found;
            if (!switchSameWindow(r, first, 0) || r->write != write || !r->sectors || r->lba < start ||
                r->sectors > sectors || r->lba - start > sectors - r->sectors)
                return false;
            total += r->sectors;
            atStart |= r->lba == start;
            atEnd |= r->lba + r->sectors == start + sectors;
            uint64_t before = start;
            for (const Request *other = head; other; other = other->next) {
                if (!other->chained || other == r)
                    continue;
                if (switchOverlaps(r, other))
                    return false;
                const uint64_t end = other->lba + other->sectors;
                if (end <= r->lba && end > before)
                    before = end;
            }
            if (r->lba - before > maxGap)
                return false;
        }
        return found == count && atStart && atEnd && total <= sectors && sectors - total <= gapBudget;
    }
    SwitchLbaRange spans[kStackCap];
    uint32_t found = 0;
    const Request *first = nullptr;
    uint64_t total = 0;
    bool sorted = true;
    for (const Request *r = head; r; r = r->next) {
        if (!r->chained)
            continue;
        if (!first)
            first = r;
        if (found >= count)
            return false;
        if (!switchSameWindow(r, first, 0) || r->write != write || !r->sectors || r->lba < start ||
            r->sectors > sectors || r->lba - start > sectors - r->sectors)
            return false;
        if (found && r->lba < spans[found - 1].lba)
            sorted = false;
        spans[found++] = {r->lba, r->sectors};
        total += r->sectors;
    }
    if (!found)
        return !count && !sectors;
    if (found != count || total > sectors || sectors - total > gapBudget)
        return false;
    if (!sorted) {
        for (uint32_t i = 1; i < found; ++i) {
            const SwitchLbaRange key = spans[i];
            uint32_t j = i;
            while (j && spans[j - 1].lba > key.lba) {
                spans[j] = spans[j - 1];
                --j;
            }
            spans[j] = key;
        }
    }
    if (spans[0].lba != start)
        return false;
    uint64_t end = spans[0].lba + spans[0].sectors;
    for (uint32_t i = 1; i < found; ++i) {
        if (spans[i].lba < end || spans[i].lba - end > maxGap)
            return false;
        end = spans[i].lba + spans[i].sectors;
    }
    return end == start + sectors;
}

// Whether the `count` requests marked `chained` are reads that fit [start, start + sectors) as a chain may: inside it,
// pairwise disjoint, one starting at `start` and one ending at its end, no gap between neighbours longer than `maxGap`
// and the gaps together at most `gapBudget`. With both 0: they tile it exactly. The driver checks this before it unlinks
// a chain (switchGatherReads is host-tested, but a chain that does not fit would put the card's bytes in the wrong
// requests).
template <typename Request>
inline bool switchChainFits(const Request *head, uint64_t start, uint32_t sectors, uint32_t count, uint32_t maxGap,
                            uint32_t gapBudget)
{
    return switchChainFitsDirection(head, start, sectors, count, maxGap, gapBudget, false);
}

// The same with no gaps: the members tile [start, start + sectors) exactly.
template <typename Request>
inline bool switchChainTiles(const Request *head, uint64_t start, uint32_t sectors, uint32_t count)
{
    return switchChainFits(head, start, sectors, count, 0, 0);
}

template <typename Request>
inline bool switchWriteChainTiles(const Request *head, uint64_t start, uint32_t sectors, uint32_t count)
{
    return switchChainFitsDirection(head, start, sectors, count, 0, 0, true);
}

// Clears every `chained` mark (a chain that failed switchChainTiles stays queued as it was).
template <typename Request> inline void switchClearChain(Request *head)
{
    for (Request *r = head; r; r = r->next)
        r->chained = false;
}

// Unlinks the `count` requests marked `chained` from the queue (head and tail kept right, the others in their order),
// clears the mark, and returns them linked in rising LBA order.
template <typename Request> inline Request *switchUnlinkChain(Request *&head, Request *&tail, uint32_t count)
{
    Request *chain = nullptr;
    Request *last = nullptr;
    Request *prev = nullptr;
    for (Request *r = head; r && count;) {
        Request *const next = r->next;
        if (!r->chained) {
            prev = r;
            r = next;
            continue;
        }
        if (prev) {
            prev->next = next;
        } else {
            head = next;
        }
        if (tail == r)
            tail = prev;
        r->chained = false;
        --count;
        if (!chain || last->lba < r->lba) {
            // The usual case: queued in rising order, appended.
            r->next = nullptr;
            if (last) {
                last->next = r;
            } else {
                chain = r;
            }
            last = r;
        } else {
            Request **at = &chain;
            while ((*at)->lba < r->lba)
                at = &(*at)->next;
            r->next = *at;
            *at = r;
        }
        r = next;
    }
    return chain;
}
