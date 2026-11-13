// SPDX-License-Identifier: MIT
#include "raw_loop_copies.h"

#include <algorithm>
#include <cstring>

namespace raw {

uint32_t LoopCopies::copies(size_t depth, size_t entries)
{
    if (!entries || !depth || depth > kMaxDepth)
        return 0;
    const uint32_t bound =
        depth == 1 ? kOutermostCopies : std::max<uint32_t>(1, kNestedCopies >> std::min<size_t>(31, 3 * (depth - 2)));
    return uint32_t(std::min<size_t>(bound, kEntryBudget / entries));
}

void LoopCopies::addWork(uint64_t work)
{
    if (!frames_.empty())
        frames_.back().work = std::min(BatchWork::kSaturated, frames_.back().work + work);
}

bool LoopCopies::bodyIntact(const PushBatches &entries, const Frame &f) const
{
    if (entries.size() - f.entriesAt != f.heads.size() + f.nestedEntries)
        return false;
    size_t head = 0;
    for (size_t i = f.entriesAt; i < entries.size(); ++i) {
        if (!(entries[i].flags & TegraGM20BPushNoPrefetch))
            return false;
        if (head < f.heads.size() && entries[i].va == f.heads[head])
            ++head;
    }
    return head == f.heads.size();
}

Predication::Answer LoopCopies::start(LoopStream s, Predication &predication, PredicateSlot slot, bool doWhile,
                                      uint64_t wordVa, const PredicateTest *test)
{
    if (frames_.size() >= kMaxDepth || (!doWhile && !test))
        return Predication::Answer::Misused;
    Frame frame;
    if (!frames_.empty()) {
        const uint32_t words = Predication::kArmWords(kMaxSegments);
        frame.arm = s.p.at;
        memset(frame.arm, 0, words * 4);
        s.p.at += words;
    }
    closeSegment(s);
    frame.entriesAt = s.entries.size();
    frame.goingVa = doWhile ? Predication::goingOf(slot) : 0;
    frames_.push_back(std::move(frame));
    openSegment(s);
    return doWhile ? predication.startDoWhile(s.p, slot) : predication.startWhile(s.p, slot, wordVa, *test);
}

Predication::Answer LoopCopies::end(LoopStream s, Predication &predication, BatchWork &batches, uint64_t batchLimit,
                                    uint64_t recordVa, uint32_t stamp, uint32_t tag, uint64_t wordVa,
                                    const PredicateTest *test)
{
    if (frames_.empty())
        return Predication::Answer::Misused;
    Frame &open = frames_.back();
    if (open.heads.size() > kMaxSegments ||
        predication.endCopy(s.p, open.heads.data(), open.heads.size(), wordVa, test) != Predication::Answer::Done)
        return Predication::Answer::Misused;
    closeSegment(s);
    if (!bodyIntact(s.entries, open))
        return Predication::Answer::Misused;
    const Frame body = std::move(open);
    frames_.pop_back();
    const size_t end = s.entries.size();
    const uint32_t n = copies(frames_.size() + 1, end - body.entriesAt);
    if (!n)
        return Predication::Answer::Misused;
    for (uint32_t c = 0; c < n; ++c) {
        if (c)
            for (size_t i = body.entriesAt; i < end; ++i)
                s.entries.append(s.entries[i], false);
        if (frames_.empty() && batches.add(body.work, batchLimit)) {
            s.entries.finishBatch();
            batches = {};
        }
    }
    addWork(BatchWork::product(body.work, n));
    if (!frames_.empty())
        frames_.back().nestedEntries += s.entries.size() - body.entriesAt;
    if (body.arm) {
        Push arm{body.arm, body.arm + Predication::kArmWords(kMaxSegments)};
        Predication::armCopies(arm, body.heads.data(), body.heads.size(), body.goingVa);
    }
    openSegment(s);
    predication.endLoop(s.p, recordVa, stamp, tag);
    return Predication::Answer::Done;
}

}  // namespace raw
