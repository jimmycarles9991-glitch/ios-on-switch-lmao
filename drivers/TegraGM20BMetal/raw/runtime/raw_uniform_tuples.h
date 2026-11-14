// SPDX-License-Identifier: MIT
// DEVELOPMENT: how many distinct tuples of uniform values Core Animation's generic merge states (..Xghfc, Pipeline::
// genericState) are drawn with, and how many draws each tuple takes, per 5 s window: whether variants specialised on
// uniform values would cover their draws. A tuple is the bytes each of the record's uniform ranges reads (PackUniformRange
// over its entry's window, raw_pack.h), never the rest of the arena a buffer is bound in.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "raw_pack.h"

namespace raw {

struct ReadRange {
    size_t first, end;
};

// The bytes uniform range `u` of `rec` reads of a buffer bound with `size` bytes past its offset: [first, end) in 16-byte
// units where the record bounds it, else the function's window on the slot, else the whole bound buffer
inline ReadRange uniformRead(const PackPipeline &rec, const PackUniformRange &u, size_t size)
{
    const PackEntry &e = rec.entry[u.entry];
    const size_t window = size_t(e.sampler | e.pad << 8) * 16;
    const size_t end = std::min(u.end ? size_t(u.end) * 16 : window ? window : size, size);
    return {std::min(size_t(u.first) * 16, end), end};
}

struct BoundBytes {
    const uint8_t *cpu;  // null: the CPU does not map them
    size_t size;
};

// The tuple of the bytes `rec`'s uniform ranges read of the buffers `boundAt(stage, slot)` gives; false: a range reads
// bytes the CPU does not map
template <class BoundAt> bool uniformTuple(const PackPipeline &rec, BoundAt boundAt, uint64_t &tuple)
{
    tuple = 0xcbf29ce484222325ull;
    for (uint32_t k = 0; k < kUniformRanges; k++) {
        const PackUniformRange &u = rec.uniform[k];
        if (u.entry >= rec.entries)
            continue;
        const PackEntry &e = rec.entry[u.entry];
        const BoundBytes b = boundAt(e.stage, e.slot);
        const ReadRange read = uniformRead(rec, u, b.size);
        if (read.end > read.first && !b.cpu)
            return false;
        for (size_t i = read.first; i < read.end; i++)
            tuple = (tuple ^ b.cpu[i]) * 0x100000001b3ull;
        tuple = (tuple ^ (uint64_t(k) << 32 | (read.end - read.first))) * 0x100000001b3ull;
    }
    return true;
}

class UniformTuples {
public:
    static constexpr uint64_t kWindowUs = 5000000;
    static constexpr size_t kTuplesPerState = 4096, kStates = 16;

    // One state's closed window
    struct Window {
        uint64_t keyHash = 0;
        uint64_t draws = 0;
        uint64_t unmapped = 0;  // draws a range of which the CPU does not map: no tuple
        uint64_t beyond = 0;    // draws of tuples first seen past kTuplesPerState: counted, not told apart
        size_t tuples = 0;
        uint64_t drawnOnce = 0, drawnUnder10 = 0, drawnUnder100 = 0, drawnMore = 0;  // tuples by their draws
        uint64_t most = 0, mostTuple = 0;  // the tuple drawn most and its draws
    };

    // A draw of state `keyHash` at `nowUs` whose ranges hash to `tuple`, or one of a range the CPU does not map; every
    // state's window that ended by `nowUs` is closed into `closed`
    void note(uint64_t keyHash, bool mapped, uint64_t tuple, uint64_t nowUs, std::vector<Window> &closed)
    {
        for (auto it = states_.begin(); it != states_.end();)
            if (nowUs - it->second.began >= kWindowUs) {
                closed.push_back(close(it->first, it->second));
                it = states_.erase(it);
            } else {
                ++it;
            }
        auto found = states_.find(keyHash);
        if (found == states_.end()) {
            if (states_.size() >= kStates)
                return;
            found = states_.emplace(keyHash, State{}).first;
            found->second.began = nowUs;
            found->second.counts.reserve(kTuplesPerState);
        }
        State &s = found->second;
        s.draws++;
        if (!mapped) {
            s.unmapped++;
            return;
        }
        auto counted = s.counts.find(tuple);
        if (counted != s.counts.end())
            counted->second++;
        else if (s.counts.size() < kTuplesPerState)
            s.counts.emplace(tuple, 1);
        else
            s.beyond++;
    }

private:
    struct State {
        uint64_t began = 0, draws = 0, unmapped = 0, beyond = 0;
        std::unordered_map<uint64_t, uint64_t> counts;
    };

    static Window close(uint64_t keyHash, const State &s)
    {
        Window w;
        w.keyHash = keyHash;
        w.draws = s.draws;
        w.unmapped = s.unmapped;
        w.beyond = s.beyond;
        w.tuples = s.counts.size();
        for (const auto &[tuple, draws] : s.counts) {
            (draws == 1 ? w.drawnOnce : draws < 10 ? w.drawnUnder10 : draws < 100 ? w.drawnUnder100 : w.drawnMore)++;
            if (draws > w.most) {
                w.most = draws;
                w.mostTuple = tuple;
            }
        }
        return w;
    }

    std::unordered_map<uint64_t, State> states_;
};

}  // namespace raw
