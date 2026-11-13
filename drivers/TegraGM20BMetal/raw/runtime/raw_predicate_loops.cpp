// SPDX-License-Identifier: MIT
// Predication's loops (raw_predication.h): each copy of a loop's body is an If on the loop's slot.
#include "raw_predication.h"

#include <cstring>
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitPredicate.h"
#include "raw_kit.h"

namespace raw {
namespace {

constexpr uint32_t kIfEqual = NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_RENDER_IF_EQUAL,
                   kIfNotEqual = NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_RENDER_IF_NOT_EQUAL;

void endHeads(Push &p, const uint64_t *heads, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        kit(p, switch_gm20b_compute_inline_word, heads[i], kLoopHeadEnded);
}

}  // namespace

// F is (live, holds): live, set once per copy under the enclosing block, never changes while the loop runs; holds is
// this copy's test
void Predication::halt(PredicateSlot slot)
{
    memset(slot.cpu, 0, kSlotBytes);
    haltVa_ = slot.va;
}

Predication::Answer Predication::startWhile(Push &p, PredicateSlot slot, uint64_t wordVa, const PredicateTest &test)
{
    const bool outermost = levels_.empty();
    if (!haltVa_)
        return Answer::Misused;
    initSlot(slot, outermost, 0, test.wordInB ? 0 : test.reference);
    const uint32_t reset[2] = {0, 0};
    kit(p, switch_gm20b_compute_inline_words, slot.va + kLiveAt, reset, 2u);
    if (outermost) {
        kit(p, switch_gm20b_render_enable, haltVa_, kIfEqual);
        kit(p, switch_gm20b_compute_inline_word, slot.va + kLiveAt, 1u);
        kit(p, switch_gm20b_render_enable_always);
    }
    if (!test.never)
        copyOperands(p, slot.va, wordVa, test);
    if (!outermost)
        markLive(p, slot);
    if (!test.never)
        testWord(p, slot.va, test, true, slot.va + kHoldsAt, 1u);
    kit(p, switch_gm20b_render_enable_always);
    kit(p, switch_gm20b_compute_idle_flushed);
    levels_.push_back({slot.va + kOperandsAt, slot.va + kLiveAt, test, Level::While, slot.cpu, wordVa});
    return Answer::Done;
}

// F is (live, going): going starts at 1 (the CPU's fill, or armCopies inside another loop) and only the copies' tests
// clear it
Predication::Answer Predication::startDoWhile(Push &p, PredicateSlot slot)
{
    const bool outermost = levels_.empty();
    initSlot(slot, outermost, 1, 0);
    if (!outermost) {
        kit(p, switch_gm20b_compute_inline_word, slot.va + kLiveAt, 0u);
        markLive(p, slot);
        kit(p, switch_gm20b_render_enable_always);
        kit(p, switch_gm20b_compute_idle_flushed);
    }
    levels_.push_back({slot.va + kOperandsAt, slot.va + kLiveAt, {}, Level::DoWhile, slot.cpu, 0});
    return Answer::Done;
}

Predication::Answer Predication::endCopy(Push &p, const uint64_t *heads, size_t count, uint64_t wordVa,
                                         const PredicateTest *test)
{
    if (levels_.empty())
        return Answer::Misused;
    Level &loop = levels_.back();
    if (loop.kind != Level::While && loop.kind != Level::DoWhile)
        return Answer::Misused;
    if (loop.kind == Level::DoWhile) {
        if (!test)
            return Answer::Misused;
        loop.test = *test;
        loop.wordVa = wordVa;
        const uint32_t reference = test->wordInB ? 0 : test->reference;
        memcpy(loop.cpu + kWordBAt, &reference, 4);
        if (test->never) {
            kit(p, switch_gm20b_compute_idle_flushed);
            kit(p, switch_gm20b_compute_inline_word, loop.liveVa + 4, 0u);
        } else {
            copyOperands(p, loop.ifVa, wordVa, *test);
            testWord(p, loop.ifVa, *test, false, loop.liveVa + 4, 0u);
            kit(p, switch_gm20b_render_enable_always);
        }
    }
    kit(p, switch_gm20b_compute_idle_flushed);
    kit(p, switch_gm20b_render_enable, loop.liveVa, kIfNotEqual);
    endHeads(p, heads, count);
    kit(p, switch_gm20b_render_enable_always);
    kit(p, switch_gm20b_compute_idle_flushed);
    kit(p, switch_gm20b_fetch_after_idle);
    return Answer::Done;
}

void Predication::endLoop(Push &p, uint64_t recordVa, uint32_t stamp, uint32_t tag)
{
    const Level loop = levels_.back();
    if (loop.kind == Level::While) {
        kit(p, switch_gm20b_compute_inline_word, loop.liveVa + 4, 0u);
        if (!loop.test.never) {
            copyOperands(p, loop.ifVa, loop.wordVa, loop.test);
            testWord(p, loop.ifVa, loop.test, true, loop.liveVa + 4, 1u);
        }
        kit(p, switch_gm20b_render_enable_always);
        kit(p, switch_gm20b_compute_idle_flushed);
    }
    kit(p, switch_gm20b_render_enable, loop.liveVa, kIfEqual);
    const uint32_t record[2] = {stamp, tag};
    kit(p, switch_gm20b_compute_inline_words, recordVa, record, 2u);
    kit(p, switch_gm20b_compute_inline_word, haltVa_, 1u);
    for (size_t i = 0; i + 1 < levels_.size(); ++i)
        kit(p, switch_gm20b_compute_inline_word, levels_[i].liveVa, 0u);
    kit(p, switch_gm20b_render_enable_always);
    levels_.pop_back();
}

void Predication::armCopies(Push &p, const uint64_t *heads, size_t count, uint64_t goingVa)
{
    for (size_t i = 0; i < count; ++i)
        kit(p, switch_gm20b_compute_inline_word, heads[i], kLoopHeadRuns);
    if (goingVa)
        kit(p, switch_gm20b_compute_inline_word, goingVa, 1u);
    kit(p, switch_gm20b_compute_idle_flushed);
    kit(p, switch_gm20b_fetch_after_idle);
}

}  // namespace raw
