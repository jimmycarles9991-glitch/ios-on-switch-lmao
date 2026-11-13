// SPDX-License-Identifier: BSD-2-Clause-Patent
// A channel's command arena and push buffer as host1x reads them, without the hardware (SwitchHost1xChannels.cpp
// drives it). Each job's stream is written in place in the arena, a ring of words, and the push buffer holds one GATHER
// per job pointing at it, as Linux's submit_gathers does (drivers/gpu/host1x/hw/channel_hw.c). Since every push buffer
// entry is two words, the RESTART that ends the ring is never inside one, and a stream that does not fit before the
// end of the arena starts again at its beginning: nothing reads the words skipped.
//
// A job's room comes back once its own fence passes (Linux update_cdma_locked frees a job's push buffer slots and
// command buffers at its syncpt_end). A stream submitted without a fence (tegra_host1x_submit_on) comes back with the
// first fenced job after it: host1x runs a channel in order, so a later job's fence means it fetched everything before.
#pragma once
#include <stdint.h>

struct SwitchHost1xArenaEntry {
    uint32_t start, end;  // words, end exclusive
    uint32_t syncpt, threshold;
    bool fenced;
};

class SwitchHost1xArena {
public:
    static constexpr uint32_t kEntries = 63;
    static constexpr uint32_t kNone = ~0u;
    static constexpr uint32_t kMaxGather = 0x3fff;  // GATHER's count field, bits 13:0

    constexpr explicit SwitchHost1xArena(uint32_t words) : size(words)
    {}

    // Room for `capacity` contiguous words, held until commit or cancel: where it starts, or kNone (another
    // reservation is open, or the jobs in flight hold the room).
    uint32_t reserve(uint32_t capacity)
    {
        if (reservedAt != kNone || !capacity || capacity > size || count == kEntries)
            return kNone;
        const uint32_t at = place(capacity);
        if (at != kNone) {
            reservedAt = at;
            reservedWords = capacity;
        }
        return at;
    }

    bool fits(uint32_t capacity) const
    {
        return reservedAt == kNone && capacity && capacity <= size && count < kEntries && place(capacity) != kNone;
    }

    bool reserved() const
    {
        return reservedAt != kNone;
    }

    // The words [first, first + words) of the reservation become a job; the room after them is given back.
    bool commit(uint32_t first, uint32_t words, bool fenced, uint32_t syncpt, uint32_t threshold)
    {
        if (reservedAt == kNone || first < reservedAt || first - reservedAt >= reservedWords || !words ||
            words > kMaxGather || words > reservedWords - (first - reservedAt))
            return false;
        entries[(oldest + count) % kEntries] = {reservedAt, first + words, syncpt, threshold, fenced};
        ++count;
        head = first + words;
        reservedAt = kNone;
        return true;
    }

    void cancel()
    {
        reservedAt = kNone;
    }

    // Retires every job up to the newest fenced one whose fence `reached(syncpt, threshold)` says passed.
    template <class Reached> uint32_t reclaim(Reached &&reached)
    {
        for (uint32_t i = count; i-- > 0;) {
            const auto &entry = entries[(oldest + i) % kEntries];
            if (!entry.fenced || !reached(entry.syncpt, entry.threshold))
                continue;
            oldest = (oldest + i + 1) % kEntries;
            count -= i + 1;
            return i + 1;
        }
        return 0;
    }

    // The channel was reset or torn down: nothing in the arena will be fetched again.
    void drop()
    {
        count = 0;
    }

    const SwitchHost1xArenaEntry *oldestFenced() const
    {
        for (uint32_t i = 0; i < count; ++i)
            if (entries[(oldest + i) % kEntries].fenced)
                return &entries[(oldest + i) % kEntries];
        return nullptr;
    }

    // Words the jobs in flight hold, skipped words included.
    uint32_t held() const
    {
        if (!count)
            return 0;
        const uint32_t tail = entries[oldest].start;
        return head > tail ? head - tail : size - tail + head;
    }

    uint32_t jobs() const
    {
        return count;
    }

private:
    const uint32_t size;
    SwitchHost1xArenaEntry entries[kEntries] = {};
    uint32_t oldest = 0, count = 0, head = 0;
    uint32_t reservedAt = kNone, reservedWords = 0;

    // Live words are [tail, head) when head > tail, else [tail, size) and [0, head); head == tail with jobs in flight
    // is full.
    uint32_t place(uint32_t capacity) const
    {
        if (!count)
            return 0;
        const uint32_t tail = entries[oldest].start;
        if (head > tail) {
            if (size - head >= capacity)
                return head;
            return tail >= capacity ? 0 : kNone;
        }
        return tail - head >= capacity ? head : kNone;
    }
};

// The push buffer's ring of two-word slots (Linux's HOST1X_PUSHBUFFER_SLOTS) and the RESTART after them. A slot is
// fetched before its job's fence can pass, so it is free again once its arena job retires: the arena's entry limit
// keeps PUT from ever reaching GET, which host1x would read as an empty ring.
struct SwitchHost1xPush {
    static constexpr uint32_t kSlots = 512;
    static constexpr uint32_t kBytes = kSlots * 8;
    static_assert(SwitchHost1xArena::kEntries < kSlots);

    static constexpr uint32_t gather(uint32_t words)
    {
        return (6u << 28) | words;
    }

    static constexpr uint32_t restart(uint32_t byteOffset)
    {
        return (5u << 28) | (byteOffset >> 4);
    }

    uint32_t put = 0;  // byte offset of the next slot

    // Writes GATHER(words) and its address at put into the ring; returns the slot's offset.
    uint32_t push(uint32_t *ring, uint32_t words, uint32_t address)
    {
        const uint32_t at = put;
        ring[at / 4] = gather(words);
        ring[at / 4 + 1] = address;
        put = (at + 8) % kBytes;
        return at;
    }
};
