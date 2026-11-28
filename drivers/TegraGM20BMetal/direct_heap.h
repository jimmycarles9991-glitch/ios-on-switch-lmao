// SPDX-License-Identifier: MIT
// What a state's reads through argument buffers take of a draw (heap_tables.h), read from m2v's plans of the probe draws
// (NativeMetalDirectDraw.rs holes_of, guest_base, NativeMetalDirectLayout.rs heap_of) for its record: each stage's
// root-table and ExecutionStatus address words, the buffer slots its root table holds, the words a buffer slot's Metal
// address goes to, and its set-1 heap arrays in NVK's layout.
#pragma once
#include <cstring>
#include <string>
#include "direct_abi.h"
#include "direct_encode.h"
#include "record_parts.h"

namespace gm20b {

inline bool sameAddressHoles(const NmbDirectDraw &a, const NmbDirectDraw &b)
{
    return !memcmp(a.rootTableWord, b.rootTableWord, sizeof a.rootTableWord) &&
           !memcmp(a.statusWord, b.statusWord, sizeof a.statusWord) && a.otherHoles == b.otherHoles &&
           !memcmp(a.guestBaseWord, b.guestBaseWord, sizeof a.guestBaseWord) &&
           !memcmp(a.guestBaseSlot, b.guestBaseSlot, sizeof a.guestBaseSlot);
}

inline bool sameHeap(const NmbDirectDraw &a, const NmbDirectDraw &b)
{
    return a.heapArrays == b.heapArrays && !memcmp(a.heapClass, b.heapClass, sizeof a.heapClass);
}

// The record's address words and root slots (the bound run's: only its buffers have addresses), and its heap; null: why not
inline const char *directHeap(const NmbDirectState &state, RecordHeap &out)
{
    const NmbDirectDraw &draw = state.draws[0];
    if (draw.otherHoles)
        return "push words m2v fills with addresses other than its root and status tables";
    for (const NmbDirectDraw &d : state.draws)
        if (!sameAddressHoles(d, draw) || !sameHeap(d, draw))
            return "probe draws on variants of other address holes or heaps";
    memcpy(out.rootTableWord, draw.rootTableWord, sizeof out.rootTableWord);
    memcpy(out.statusWord, draw.statusWord, sizeof out.statusWord);
    static_assert(sizeof out.guestBaseWord == sizeof draw.guestBaseWord, "pack.h kPackGuestBases");
    memcpy(out.guestBaseWord, draw.guestBaseWord, sizeof out.guestBaseWord);
    memcpy(out.guestBaseSlot, draw.guestBaseSlot, sizeof out.guestBaseSlot);
    memcpy(out.rootSlots, state.draws[3].rootSlots, sizeof out.rootSlots);
    for (uint32_t k = 0; k < 2; k++)
        if (out.rootSlots[k] && !out.rootTableWord[k])
            return "a root table's buffer slots in a stage that reads no root table";
    if (!draw.heapArrays)
        return nullptr;
    if (const char *refused = heapSetArrays(state.layoutData, draw.heapArrays, draw.heapClass, out.array, out.bytes))
        return refused;
    out.arrays = uint8_t(draw.heapArrays);
    return nullptr;
}

}  // namespace gm20b
