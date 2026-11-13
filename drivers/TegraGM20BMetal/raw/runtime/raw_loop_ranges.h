// SPDX-License-Identifier: MIT
// The submission's view of a command buffer's predicated loops (raw_loop_copies.h). A native range splits after each
// block that holds a loop and no block holds, so what follows waits for that block's job. Where a loop recorded that it
// would run past its copies (Predication::endLoop), what is laid down next is the loop, then the rest of each block
// around it, innermost first, with an enclosing loop laid again after its rest. The blocks around it ran, so each piece
// runs unconditionally, and its own blocks test their words again. Blocks open and close as the records' units keep
// them (raw_predicate_records.h): a compute encoder of another dispatch type, other work, an event or the command
// buffer's end closes every block still open.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "raw_predicate_records.h"

namespace raw {

// A tape position as these walks see it
struct LoopTapeEntry {
    enum Kind : uint8_t {
        Plain,
        Compute,    // a compute encoder's start: `value` its dispatch type
        Other,      // any other encoder or an event
        Predicate,  // a predication record: `value` its PredicateRecords::Kind
    };

    Kind kind = Plain;
    uint32_t value = 0;
};

struct TapeRange {
    uint32_t from = 0, to = 0;

    bool operator==(const TapeRange &o) const
    {
        return from == o.from && to == o.to;
    }
};

// What a retired range's loop record says: a loop of the command buffer whose stamp it holds ran past its copies, the
// loop whose End record is at `tag`
struct LoopContinue {
    bool continues = false;
    uint32_t tag = 0;
};

inline LoopContinue loopContinue(uint32_t stamp, uint32_t recordStamp, uint32_t recordTag)
{
    if (!stamp || recordStamp != stamp)
        return {};
    return {true, recordTag};
}

// The blocks open at each position of a walk over [from, to)
class LoopBlocks {
public:
    struct Block {
        bool loop = false;
        bool inElse = false;  // an If's Else part
        bool holdsLoop = false;
        uint32_t startAt = 0, elseAt = 0;
    };

    // Where a block closed: after its End record (`byEnd`), or at the position that closed its unit
    struct Closed {
        Block block;
        uint32_t at = 0;
        bool byEnd = false;
        size_t depth = 0;  // the blocks still open around it
    };

    // A walk from `from`, which may lie inside a unit: the unit is its last compute encoder's, unless other work came after
    template <class Tape> LoopBlocks(const Tape &tape, uint32_t from)
    {
        for (uint32_t at = from; at-- > 0;) {
            const LoopTapeEntry e = tape(at);
            if (e.kind == LoopTapeEntry::Compute) {
                unit_ = true;
                type_ = e.value;
            }
            if (e.kind == LoopTapeEntry::Compute || e.kind == LoopTapeEntry::Other)
                return;
        }
    }

    // The entry at `at`; the blocks it closed, innermost first
    template <class OnClose> void step(uint32_t at, const LoopTapeEntry &e, OnClose &&closed)
    {
        if (e.kind == LoopTapeEntry::Compute) {
            if (!unit_ || type_ != e.value)
                closeAll(at, closed);
            unit_ = true;
            type_ = e.value;
        } else if (e.kind == LoopTapeEntry::Other) {
            closeAll(at, closed);
            unit_ = false;
        } else if (e.kind == LoopTapeEntry::Predicate) {
            record(at, PredicateRecords::Kind(e.value), closed);
        }
    }

    const std::vector<Block> &open() const
    {
        return open_;
    }

private:
    template <class OnClose> void closeAll(uint32_t at, OnClose &&closed)
    {
        while (!open_.empty()) {
            const Block b = open_.back();
            open_.pop_back();
            closed(Closed{b, at, false, open_.size()});
        }
    }

    template <class OnClose> void record(uint32_t at, PredicateRecords::Kind k, OnClose &&closed)
    {
        using R = PredicateRecords;
        if (k == R::StartIf || k == R::StartWhile || k == R::StartDoWhile) {
            const bool loop = k != R::StartIf;
            if (loop)
                for (Block &b : open_)
                    b.holdsLoop = true;
            open_.push_back({loop, false, loop, at, 0});
        } else if (k == R::StartElse) {
            if (!open_.empty() && !open_.back().loop && !open_.back().inElse) {
                open_.back().inElse = true;
                open_.back().elseAt = at;
            }
        } else if (!open_.empty() && open_.back().loop == (k != R::EndIf)) {
            const Block b = open_.back();
            open_.pop_back();
            closed(Closed{b, at + 1, true, open_.size()});
        }
    }

    std::vector<Block> open_;
    uint32_t type_ = 0;
    bool unit_ = false;
};

// Where [from, to) splits: after each block that holds a loop and no block holds
template <class Tape> std::vector<uint32_t> loopSplits(const Tape &tape, uint32_t from, uint32_t to)
{
    std::vector<uint32_t> splits;
    LoopBlocks blocks(tape, from);
    for (uint32_t at = from; at < to; ++at)
        blocks.step(at, tape(at), [&](const LoopBlocks::Closed &c) {
            if (!c.depth && c.block.holdsLoop && c.at > from && c.at < to && (splits.empty() || splits.back() != c.at))
                splits.push_back(c.at);
        });
    return splits;
}

// Whether [from, to) lays a loop down
template <class Tape> bool laysLoops(const Tape &tape, uint32_t from, uint32_t to)
{
    for (uint32_t at = from; at < to; ++at) {
        const LoopTapeEntry e = tape(at);
        if (e.kind == LoopTapeEntry::Predicate &&
            (e.value == PredicateRecords::StartWhile || e.value == PredicateRecords::StartDoWhile))
            return true;
    }
    return false;
}

// What [from, to) lays down next after the loop whose End record is at `endAt` ran past its copies: the loop, then the
// rest of each block around it inside the range (an If's to its Else where the loop sat before it, a loop's then the
// whole loop again). Empty: `endAt` ends no loop in the range.
template <class Tape> std::vector<TapeRange> continuation(const Tape &tape, uint32_t from, uint32_t to, uint32_t endAt)
{
    LoopBlocks blocks(tape, from);
    const auto ignore = [](const LoopBlocks::Closed &) {};
    for (uint32_t at = from; at < endAt && at < to; ++at)
        blocks.step(at, tape(at), ignore);
    const LoopTapeEntry end = tape(endAt);
    if (endAt >= to || blocks.open().empty() || !blocks.open().back().loop || end.kind != LoopTapeEntry::Predicate ||
        (end.value != PredicateRecords::EndWhile && end.value != PredicateRecords::EndDoWhile))
        return {};
    const std::vector<LoopBlocks::Block> chain = blocks.open();
    const size_t enclosing = chain.size() - 1;
    std::vector<LoopBlocks::Closed> closes(enclosing);
    std::vector<uint32_t> elseAt(enclosing, 0);
    std::vector<bool> closed(enclosing, false);
    for (uint32_t at = endAt; at < to; ++at) {
        const size_t before = blocks.open().size();
        blocks.step(at, tape(at), [&](const LoopBlocks::Closed &c) {
            if (c.depth < enclosing && !closed[c.depth]) {
                closes[c.depth] = c;
                closed[c.depth] = true;
            }
        });
        const auto &open = blocks.open();
        const size_t k = open.size() - 1;
        if (at > endAt && open.size() == before && !open.empty() && k < enclosing && !closed[k] &&
            open.back().startAt == chain[k].startAt && open.back().elseAt == at && !chain[k].inElse)
            elseAt[k] = at;
    }
    std::vector<TapeRange> pieces{{chain.back().startAt, endAt + 1}};
    uint32_t childEnd = endAt + 1;
    for (size_t k = enclosing; k-- > 0;) {
        const LoopBlocks::Block &b = chain[k];
        const uint32_t closeAt = closed[k] ? closes[k].at : to;
        const uint32_t bodyEnd = closed[k] && closes[k].byEnd ? closeAt - 1 : closeAt;
        const uint32_t stop = elseAt[k] ? elseAt[k] : bodyEnd;
        if (childEnd < stop)
            pieces.push_back({childEnd, stop});
        if (b.loop && closed[k] && closes[k].byEnd)
            pieces.push_back({b.startAt, closeAt});
        childEnd = closeAt;
    }
    return pieces;
}

}  // namespace raw
