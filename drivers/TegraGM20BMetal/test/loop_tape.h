// SPDX-License-Identifier: MIT
// A command buffer's compute work as a tape of dispatches and predication records over a few words, and Metal's answer for
// it: the tape walked on the CPU. Each dispatch's program adds `delta` to word `w` and may set another word: to `value`, or
// with `flag` to whether word `w` is still non-zero, as MPSGraph's condition kernels write a loop's 0/1 word.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <vector>

#include "drivers/TegraGM20BMetal/raw/runtime/raw_loop_ranges.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_predication.h"

namespace loop_tape {

using raw::PredicateCompare;

constexpr uint32_t kWordCount = 8;

using Words = std::array<uint32_t, kWordCount>;

struct Op {
    // Encoder: the compute encoder ends and another of its dispatch type begins
    enum Kind : uint8_t {
        Dispatch,
        If,
        Else,
        EndIf,
        While,
        EndWhile,
        DoWhile,
        EndDoWhile,
        Encoder
    } kind;

    uint32_t w = 0;
    PredicateCompare compare = PredicateCompare::NotEqual;
    uint32_t reference = 0;
    int32_t delta = 0;
    uint32_t set = kWordCount, value = 0;  // `set` kWordCount: none
    bool flag = false;
};

using Tape = std::vector<Op>;

inline Op dispatch(uint32_t w, int32_t delta, uint32_t set = kWordCount, uint32_t value = 0)
{
    return {Op::Dispatch, w, {}, 0, delta, set, value};
}

inline Op countdown(uint32_t w, uint32_t flag, int32_t delta = -1)
{
    Op op = dispatch(w, delta, flag);
    op.flag = true;
    return op;
}

inline Op condition(uint32_t w, uint32_t flag)
{
    return countdown(w, flag, 0);
}

inline Op test(Op::Kind k, uint32_t w, PredicateCompare c = PredicateCompare::NotEqual, uint32_t reference = 0)
{
    return {k, w, c, reference};
}

inline void effect(const Op &op, Words &words)
{
    words[op.w] += uint32_t(op.delta);
    if (op.set < kWordCount)
        words[op.set] = op.flag ? uint32_t(words[op.w] != 0) : op.value;
}

inline bool holds(const Op &op, const Words &words)
{
    return raw::predicateRuns(raw::predicateTest(op.compare, op.reference), words[op.w]);
}

// The indices of the dispatches Metal runs, in order
inline std::vector<uint32_t> metal(const Tape &tape, Words &words)
{
    std::vector<uint32_t> ran;
    std::map<uint32_t, uint32_t> match;
    std::vector<uint32_t> open;
    for (uint32_t i = 0; i < tape.size(); ++i) {
        const Op::Kind k = tape[i].kind;
        if (k == Op::If || k == Op::While || k == Op::DoWhile) {
            open.push_back(i);
        } else if (k == Op::Else) {
            match[open.back()] = i;
            match[i] = open.back();
        } else if (k == Op::EndIf || k == Op::EndWhile || k == Op::EndDoWhile) {
            const uint32_t s = open.back();
            open.pop_back();
            if (!match.count(s))
                match[s] = i;
            else
                match[match[s]] = i;
            match[i] = s;
        }
    }
    uint32_t pc = 0;
    uint64_t steps = 0;
    while (pc < tape.size() && ++steps < 10'000'000) {
        const Op &op = tape[pc];
        switch (op.kind) {
        case Op::Dispatch:
            ran.push_back(pc);
            effect(op, words);
            ++pc;
            break;
        case Op::If:
        case Op::While:
            pc = holds(op, words) ? pc + 1 : match[pc] + 1;
            break;
        case Op::Else:
            pc = match[pc] + 1;
            break;
        case Op::EndWhile:
            pc = match[pc];
            break;
        case Op::DoWhile:
        case Op::EndIf:
        case Op::Encoder:
            ++pc;
            break;
        case Op::EndDoWhile:
            pc = holds(op, words) ? match[pc] + 1 : pc + 1;
            break;
        }
    }
    return ran;
}

inline raw::LoopTapeEntry entry(const Op &op)
{
    using Records = raw::PredicateRecords;
    static const Records::Kind kinds[] = {Records::StartIf,      Records::StartIf,    Records::StartElse,
                                          Records::EndIf,        Records::StartWhile, Records::EndWhile,
                                          Records::StartDoWhile, Records::EndDoWhile};
    if (op.kind == Op::Dispatch)
        return {};
    if (op.kind == Op::Encoder)
        return {raw::LoopTapeEntry::Compute, 0};
    return {raw::LoopTapeEntry::Predicate, uint32_t(kinds[op.kind])};
}

}  // namespace loop_tape
