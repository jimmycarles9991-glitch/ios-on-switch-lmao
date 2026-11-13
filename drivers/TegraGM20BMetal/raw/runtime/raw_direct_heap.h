// SPDX-License-Identifier: MIT
// What a state's reads through argument buffers take of a draw (raw_heap_tables.h), read from m2v's plans of the probe draws
// (NativeMetalDirect.rs holes_of, heap_of) for its record: each stage's root-table and ExecutionStatus address words, the
// buffer slots its root table holds, and its set-1 heap arrays in NVK's layout.
#pragma once
#include <cstring>
#include <string>
#include "bridge/NativeMetalDirect.h"
#include "raw_direct_encode.h"
#include "raw_record_parts.h"

namespace raw {

inline bool sameAddressHoles(const NmbDirectDraw &a, const NmbDirectDraw &b)
{
    return !memcmp(a.rootTableWord, b.rootTableWord, sizeof a.rootTableWord) &&
           !memcmp(a.statusWord, b.statusWord, sizeof a.statusWord) && a.otherHoles == b.otherHoles;
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

}  // namespace raw
