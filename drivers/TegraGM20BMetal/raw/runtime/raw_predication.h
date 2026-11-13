// SPDX-License-Identifier: MIT
// Metal's command predication (encodeStartIf:offset:comparison:referenceValue:, encodeStartElse, encodeEndIf on compute
// encoders; Metal's rules as m2v measured them on an M4: metal2vulkan-runtime/src/predication.rs) on the compute class's
// render enable (SwitchGM20BEmitPredicate.h), which only tests two 64-bit operands for equality. Each record takes a 64-byte
// slot of the command buffer's upload memory:
//   +0  A: the word, copied by the copy engine; the ordered compares fold the reference in with the copy's MIN/MAX release
//   +16 B: the reference, or the word again for the ordered compares
//   +32 F: low word 1 where the enclosing block runs, high word 1 where this block's test holds, each set by an inline
//          write that render enable gates
//   +48 K: 1 in both words
// A launch inside a block runs where F equals K. The enable is set around each launch and put back to always after it, so
// nothing else on the channel (3D work, raw's own inline writes, the next job) ever runs under it.
//
// Loops (encodeStartWhile/EndWhile, encodeStartDoWhile/EndDoWhile). GM20B's channel has no backward jump: the host class
// has no jump, call or return (clb06f.h DMA_SEC_OP 0-7: method headers and END_PB_SEGMENT), a GP entry only names a
// segment (GP_ENTRY1_LEVEL is progress reporting, FETCH_CONDITIONAL is SLI's subdevice status), the GPU cannot add GP
// entries (gm20b takes GP_PUT through BAR1: nvgpu hal_gm20b.c userd_gp_put), and the MME is the 3D class's alone and reads
// no memory before Turing (clb1c0.h has no CALL_MME_MACRO, clc597.h adds MME_DMA_READ). The compute scheduler's own
// chaining (QMDV01_07 DEPENDENT_QMD_POINTER, clb1c0qmd.h:248) is written by no open driver. So a loop's body is laid down
// once and its segments listed as GP entries once per copy (raw_loop_copies.h). Each copy runs as an If on the loop's slot:
// a While's copy tests the word at its head, a DoWhile's at its end for the next copy, and each copy's launches run where
// its F equals K. Where a copy does not run, its end writes END_PB_SEGMENT over the first word of each of the body's
// segments, which every copy's GP entry fetches again after the host waited for idle (NO_PREFETCH), so the copies after
// the loop's end cost a fetch each. The copies stay right without that: each one's own test fails again.
#pragma once
#include <cstdint>
#include <vector>
#include "raw_push.h"

namespace raw {

enum class PredicateCompare : uint8_t {
    Equal,
    NotEqual,
    Greater,
    GreaterEqual,
    Less,
    LessEqual,
};

// Metal's `comparison:`: 0 to 5, any other value compares equal (m2v measured 6, 255, 2^32 + 2, 2^64 - 1)
PredicateCompare predicateCompare(uint64_t comparison);

// `word OP reference` (unsigned) as the operands and the equality the unit tests
struct PredicateTest {
    bool never = false;    // no word satisfies it: nothing is copied
    bool wordInB = false;  // B is the word too, else `reference`
    bool reduce = false;   // A = reduction(word, payload)
    uint32_t reduction = 0;
    uint32_t payload = 0;
    uint32_t reference = 0;
    bool equal = true;  // RENDER_IF_EQUAL, else RENDER_IF_NOT_EQUAL
};

PredicateTest predicateTest(PredicateCompare compare, uint32_t reference);

// What the unit decides for `word` under `test`: the copy, the reduction and the 64-bit compare
bool predicateRuns(const PredicateTest &test, uint32_t word);

struct PredicateSlot {
    uint8_t *cpu = nullptr;
    uint64_t va = 0;
};

// A segment's first word as the copies of a loop find it: a NOP while the loop runs, END_PB_SEGMENT once it ended
// (open-gpu-doc dev_ram.ref FIFO_DMA: NV_FIFO_DMA_NOP is 0, "a universal NOP entry"; after END_PB_SEGMENT "no further PB
// entries from that PB segment will be decoded or processed")
constexpr uint32_t kLoopHeadRuns = 0, kLoopHeadEnded = 7u << 29;

class Predication {
public:
    static constexpr uint32_t kSlotBytes = 64, kSlotAlign = 16;
    static constexpr uint32_t kStartWords = 112, kGuardWords = 4, kReleaseWords = 4;

    // endLoop's words inside `enclosing` blocks and loops
    static constexpr uint32_t kOverflowWords(size_t enclosing)
    {
        return 96 + 8 * uint32_t(enclosing);
    }

    static constexpr uint32_t kEndCopyWords(size_t heads)
    {
        return 80 + 8 * uint32_t(heads);
    }

    static constexpr uint32_t kArmWords(size_t heads)
    {
        return 16 + 8 * uint32_t(heads);
    }

    enum class Answer : uint8_t {
        Done,
        Misused,  // an Else with no open If or after another Else, an End with no open block: Metal answers NO
    };

    Answer startIf(Push &p, PredicateSlot slot, uint64_t wordVa, const PredicateTest &test);
    Answer startElse(Push &p, PredicateSlot slot);
    Answer endIf();

    // The head of each copy of a While: the word's test for this copy, on `slot` (the loop's, every copy's)
    Answer startWhile(Push &p, PredicateSlot slot, uint64_t wordVa, const PredicateTest &test);
    // The head of each copy of a DoWhile: its launches run while the loop goes
    Answer startDoWhile(Push &p, PredicateSlot slot);
    // The end of each copy: a DoWhile's test for the next copy (`wordVa`, `test`), then END_PB_SEGMENT into `heads` where
    // the next copy does not run, and the host's wait after which the fetcher sees them. Misused: no loop is the open block
    Answer endCopy(Push &p, const uint64_t *heads, size_t count, uint64_t wordVa = 0,
                   const PredicateTest *test = nullptr);
    // After the last copy, where the loop would run another copy: `stamp` and `tag` into the record at `recordVa`, and
    // every enclosing block's F out of its K, so nothing more runs in them (later copies of an enclosing loop end at
    // its copy's end; an enclosing If's Else finds its test failed). The submission then lays the loop again and the
    // rest of its enclosing blocks after it (raw_loop_ranges.h). The loop's block closes
    void endLoop(Push &p, uint64_t recordVa, uint32_t stamp, uint32_t tag);
    // Before the first copy of a loop inside another loop's copy: its heads back to NOP, a DoWhile's `going` back to 1
    // (`goingVa`, 0 for a While), seen by the fetcher before the first copy
    static void armCopies(Push &p, const uint64_t *heads, size_t count, uint64_t goingVa);

    // Metal's unit ended (another encoder type or dispatch type, an event, the command buffer's end): a block still open
    // runs nothing more
    void endUnit()
    {
        levels_.clear();
        haltVa_ = 0;
    }

    // The unit's halt operands (kSlotBytes, the CPU's fill equal): a loop past its copies makes them unequal, and no
    // outermost While's later copy runs from then on, even where the fetcher reads a copy whose head the GPU ended
    void halt(PredicateSlot slot);

    bool halting() const
    {
        return haltVa_ != 0;
    }

    bool active() const
    {
        return !levels_.empty();
    }

    size_t depth() const
    {
        return levels_.size();
    }

    bool inLoop() const
    {
        for (const Level &l : levels_)
            if (l.kind == Level::While || l.kind == Level::DoWhile)
                return true;
        return false;
    }

    // A DoWhile's `going` word: its slot's F high word
    static uint64_t goingOf(const PredicateSlot &slot)
    {
        return slot.va + kHoldsAt;
    }

    // Around each launch inside a block
    void guard(Push &p) const;
    void release(Push &p) const;

private:
    static constexpr uint32_t kOperandsAt = 0, kWordBAt = 16, kLiveAt = 32, kHoldsAt = 36, kKeyAt = 48;

    struct Level {
        enum Kind : uint8_t {
            If,
            Else,
            While,
            DoWhile,
        };

        uint64_t ifVa;    // the slot's operands
        uint64_t liveVa;  // this branch's F/K pair
        PredicateTest test;
        Kind kind;
        uint8_t *cpu = nullptr;  // a loop's slot, and the word its test reads
        uint64_t wordVa = 0;
    };

    void initSlot(PredicateSlot slot, uint32_t live, uint32_t holds, uint32_t reference) const;
    void markLive(Push &p, PredicateSlot slot) const;
    // Inside a loop's copy a slot's F starts each copy as `live`, `holds`: the CPU's fill is the first copy's only
    void resetInLoop(Push &p, PredicateSlot slot, uint32_t live, uint32_t holds) const;
    static void copyOperands(Push &p, uint64_t slotVa, uint64_t wordVa, const PredicateTest &test);
    static void testWord(Push &p, uint64_t slotVa, const PredicateTest &test, bool holds, uint64_t va, uint32_t value);

    std::vector<Level> levels_;
    uint64_t haltVa_ = 0;
};

}  // namespace raw
