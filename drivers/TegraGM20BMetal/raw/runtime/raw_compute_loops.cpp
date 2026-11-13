// SPDX-License-Identifier: MIT
// The compute encoder's predicated loops (raw_loop_copies.h).
#include "raw_compute.h"

namespace raw {

Predication::Answer ComputeEncoder::startWhile(uint64_t wordVa, PredicateCompare compare, uint32_t reference)
{
    const PredicateTest test = predicateTest(compare, reference);
    return startLoop(false, wordVa, &test);
}

Predication::Answer ComputeEncoder::startDoWhile()
{
    return startLoop(true, 0, nullptr);
}

Predication::Answer ComputeEncoder::endWhile(uint32_t tag)
{
    return endLoop(0, nullptr, tag);
}

Predication::Answer ComputeEncoder::endDoWhile(uint64_t wordVa, PredicateCompare compare, uint32_t reference,
                                               uint32_t tag)
{
    const PredicateTest test = predicateTest(compare, reference);
    return endLoop(wordVa, &test, tag);
}

Predication::Answer ComputeEncoder::startLoop(bool doWhile, uint64_t wordVa, const PredicateTest *test)
{
    room(LoopCopies::kStartRoom);
    uint64_t va;
    if (!cb_.predication_.halting()) {
        uint8_t *halt = dev_.alloc(dev_.upload_, Predication::kSlotBytes, Predication::kSlotAlign, va);
        cb_.predication_.halt({halt, va});
    }
    uint8_t *cpu = dev_.alloc(dev_.upload_, Predication::kSlotBytes, Predication::kSlotAlign, va);
    return cb_.loops_.start(stream(), cb_.predication_, {cpu, va}, doWhile, wordVa, test);
}

Predication::Answer ComputeEncoder::endLoop(uint64_t wordVa, const PredicateTest *test, uint32_t tag)
{
    room(LoopCopies::endRoom(cb_.predication_.depth()));
    const Predication::Answer answer = cb_.loops_.end(stream(), cb_.predication_, cb_.batchWork_, cb_.batchLimit(),
                                                      dev_.loopRecordVa(cb_.stamp), cb_.stamp, tag, wordVa, test);
    cb_.loopsLaid |= answer == Predication::Answer::Done;
    return answer;
}

}  // namespace raw
