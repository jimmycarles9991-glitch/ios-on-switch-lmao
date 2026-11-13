// SPDX-License-Identifier: MIT
#include "raw_predication.h"

#include <algorithm>
#include <cstring>
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitPredicate.h"
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitReport.h"
#include "raw_kit.h"

namespace raw {
namespace {

constexpr uint32_t kIfEqual = NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_RENDER_IF_EQUAL,
                   kIfNotEqual = NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_RENDER_IF_NOT_EQUAL;

uint32_t mode(bool equal)
{
    return equal ? kIfEqual : kIfNotEqual;
}

}  // namespace

PredicateCompare predicateCompare(uint64_t comparison)
{
    return comparison <= 5 ? PredicateCompare(comparison) : PredicateCompare::Equal;
}

PredicateTest predicateTest(PredicateCompare compare, uint32_t reference)
{
    PredicateTest t;
    t.reference = reference;
    auto fold = [&](uint32_t reduction, uint32_t payload) {
        t.wordInB = true;
        t.reduce = true;
        t.reduction = reduction;
        t.payload = payload;
    };
    switch (compare) {
    case PredicateCompare::Equal:
        break;
    case PredicateCompare::NotEqual:
        t.equal = false;
        break;
    case PredicateCompare::LessEqual:
        fold(NVB0B5_LAUNCH_DMA_SEMAPHORE_REDUCTION_IMIN, reference);
        break;
    case PredicateCompare::Less:
        t.never = reference == 0;
        fold(NVB0B5_LAUNCH_DMA_SEMAPHORE_REDUCTION_IMIN, reference - 1);
        break;
    case PredicateCompare::GreaterEqual:
        fold(NVB0B5_LAUNCH_DMA_SEMAPHORE_REDUCTION_IMAX, reference);
        break;
    case PredicateCompare::Greater:
        t.never = reference == UINT32_MAX;
        fold(NVB0B5_LAUNCH_DMA_SEMAPHORE_REDUCTION_IMAX, reference + 1);
        break;
    }
    return t;
}

bool predicateRuns(const PredicateTest &t, uint32_t word)
{
    if (t.never)
        return false;
    uint64_t a = word;
    const uint64_t b = t.wordInB ? word : t.reference;
    if (t.reduce)
        a = t.reduction == NVB0B5_LAUNCH_DMA_SEMAPHORE_REDUCTION_IMIN ? std::min(word, t.payload)
                                                                      : std::max(word, t.payload);
    return t.equal ? a == b : a != b;
}

void Predication::copyOperands(Push &p, uint64_t slotVa, uint64_t wordVa, const PredicateTest &test)
{
    kit(p, switch_gm20b_compute_idle_flushed);
    if (test.reduce)
        kit(p, switch_gm20b_copy_word_reduce, wordVa, slotVa + kOperandsAt, test.payload, test.reduction, false);
    else
        kit(p, switch_gm20b_copy_bytes, wordVa, slotVa + kOperandsAt, 4u,
            uint8_t(NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED));
    if (test.wordInB)
        kit(p, switch_gm20b_copy_bytes, wordVa, slotVa + kWordBAt, 4u,
            uint8_t(NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED));
    kit(p, switch_gm20b_compute_idle_flushed);
}

void Predication::testWord(Push &p, uint64_t slotVa, const PredicateTest &test, bool holds, uint64_t va, uint32_t value)
{
    kit(p, switch_gm20b_render_enable, slotVa + kOperandsAt, mode(holds ? test.equal : !test.equal));
    kit(p, switch_gm20b_compute_inline_word, va, value);
}

void Predication::resetInLoop(Push &p, PredicateSlot slot, uint32_t live, uint32_t holds) const
{
    if (!inLoop())
        return;
    const uint32_t words[2] = {live, holds};
    kit(p, switch_gm20b_compute_inline_words, slot.va + kLiveAt, words, 2u);
}

void Predication::initSlot(PredicateSlot slot, uint32_t live, uint32_t holds, uint32_t reference) const
{
    const uint32_t words[16] = {0, 0, 0, 0, reference, 0, 0, 0, live, holds, 0, 0, 1, 1, 0, 0};
    static_assert(sizeof words == kSlotBytes);
    static_assert(kKeyAt == 12 * 4 && kHoldsAt == kLiveAt + 4 && kWordBAt == 4 * 4);
    memcpy(slot.cpu, words, sizeof words);
}

void Predication::markLive(Push &p, PredicateSlot slot) const
{
    kit(p, switch_gm20b_render_enable, levels_.back().liveVa, kIfEqual);
    kit(p, switch_gm20b_compute_inline_word, slot.va + kLiveAt, 1u);
}

Predication::Answer Predication::startIf(Push &p, PredicateSlot slot, uint64_t wordVa, const PredicateTest &test)
{
    const bool outermost = levels_.empty();
    initSlot(slot, outermost, 0, test.wordInB ? 0 : test.reference);
    resetInLoop(p, slot, 0, 0);
    if (!test.never)
        copyOperands(p, slot.va, wordVa, test);
    if (!outermost)
        markLive(p, slot);
    if (!test.never)
        testWord(p, slot.va, test, true, slot.va + kHoldsAt, 1u);
    if (!outermost || !test.never) {
        kit(p, switch_gm20b_render_enable_always);
        kit(p, switch_gm20b_compute_idle_flushed);
    }
    levels_.push_back({slot.va + kOperandsAt, slot.va + kLiveAt, test, Level::If, slot.cpu, wordVa});
    return Answer::Done;
}

Predication::Answer Predication::startElse(Push &p, PredicateSlot slot)
{
    if (levels_.empty() || levels_.back().kind != Level::If)
        return Answer::Misused;
    const Level open = levels_.back();
    levels_.pop_back();
    const bool outermost = levels_.empty();
    initSlot(slot, outermost, open.test.never, 0);
    resetInLoop(p, slot, 0, open.test.never);
    if (!outermost)
        markLive(p, slot);
    if (!open.test.never) {
        kit(p, switch_gm20b_render_enable, open.ifVa, mode(!open.test.equal));
        kit(p, switch_gm20b_compute_inline_word, slot.va + kHoldsAt, 1u);
    }
    if (!outermost || !open.test.never) {
        kit(p, switch_gm20b_render_enable_always);
        kit(p, switch_gm20b_compute_idle_flushed);
    }
    levels_.push_back({open.ifVa, slot.va + kLiveAt, open.test, Level::Else, slot.cpu, open.wordVa});
    return Answer::Done;
}

Predication::Answer Predication::endIf()
{
    if (levels_.empty() || (levels_.back().kind != Level::If && levels_.back().kind != Level::Else))
        return Answer::Misused;
    levels_.pop_back();
    return Answer::Done;
}

void Predication::guard(Push &p) const
{
    if (!levels_.empty())
        kit(p, switch_gm20b_render_enable, levels_.back().liveVa, kIfEqual);
}

void Predication::release(Push &p) const
{
    if (!levels_.empty())
        kit(p, switch_gm20b_render_enable_always);
}

}  // namespace raw
