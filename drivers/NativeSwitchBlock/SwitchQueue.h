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
                                  uint64_t *start, uint32_t *sectors, uint32_t maxGap = 0, uint32_t gapBudget = 0)
{
    first->chained = true;
    uint64_t low = first->lba;
    uint64_t high = first->lba + first->sectors;
    uint64_t payload = first->sectors;
    uint32_t count = 1;
    // Passes over the queue until one adds nothing: requests queued in rising order join in one pass.
    for (bool grew = true; grew && count < maxCount;) {
        grew = false;
        unsigned seen = 0;
        for (Request *r = head; r && seen < scan && count < maxCount; r = r->next, ++seen) {
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
                inside = true;  // fills (part of) a gap, if it overlaps no member
            } else {
                continue;
            }
            if (gap > maxGap || newHigh - newLow > limit)
                continue;
            bool overlaps = false;
            for (const Request *older = head; older != r && !overlaps; older = older->next)
                overlaps = !older->chained && switchOverlaps(older, r);
            for (const Request *member = head; inside && member && !overlaps; member = member->next)
                overlaps = member->chained && switchOverlaps(member, r);
            if (overlaps || (newHigh - newLow) - (payload + r->sectors) > gapBudget)
                continue;
            r->chained = true;
            ++count;
            payload += r->sectors;
            low = newLow;
            high = newHigh;
            grew = true;
        }
    }
    *start = low;
    *sectors = static_cast<uint32_t>(high - low);
    return count;
}

// `inOrder`: a synchronize waits on this window's writes. Its barrier counts completions (SwitchSdWriteFence.h), so
// while it waits no write may join past an older write still queued: one submitted after the barrier would complete in
// the place of one it waits for. With no barrier waiting, any barrier that begins later counts both writes. This keeps
// the chain a prefix of the window's queued writes only because the pick starts a write chain with the window's oldest
// queued write; a pick that let `first` pass an older write would break the fence with or without this rule.
template <typename Request>
inline uint32_t switchGatherWrites(Request *head, Request *first, uint32_t limit, uint32_t maxCount, unsigned scan,
                                   uint64_t *start, uint32_t *sectors, const SwitchLbaRange *inFlight = nullptr,
                                   unsigned inFlightCount = 0, bool inOrder = false)
{
    first->chained = true;
    uint64_t low = first->lba;
    uint64_t high = first->lba + first->sectors;
    uint32_t count = 1;
    for (bool grew = true; grew && count < maxCount;) {
        grew = false;
        unsigned seen = 0;
        for (Request *r = head; r && seen < scan && count < maxCount; r = r->next, ++seen) {
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
    *start = low;
    *sectors = static_cast<uint32_t>(high - low);
    return count;
}

template <typename Request>
inline bool switchChainFitsDirection(const Request *head, uint64_t start, uint32_t sectors, uint32_t count,
                                     uint32_t maxGap, uint32_t gapBudget, bool write)
{
    if (write && (maxGap || gapBudget))
        return false;
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
    if (!found)
        return !count && !sectors;
    return found == count && atStart && atEnd && total <= sectors && sectors - total <= gapBudget;
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
