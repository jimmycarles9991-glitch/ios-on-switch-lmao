// SPDX-License-Identifier: MIT
#include "plan_cache.h"
#include "plan_key.h"

namespace raw::plan {

const Plan &PlanCache::get(const Recording &r, const Caps &caps, const Answers &answers)
{
    const uint64_t key = structuralKey(r, caps) ^
                         (uint64_t(answers.layeredRendering) << 60 | uint64_t(answers.rateMaps) << 61 |
                          uint64_t(answers.samplePositions) << 62 | uint64_t(answers.nonUniformThreadgroups) << 63);
    if (const auto found = entries_.find(key); found != entries_.end()) {
        ++hits_;
        lru_.splice(lru_.begin(), lru_, found->second);
        return found->second->second;
    }
    ++plans_;
    Plan p;
    const JobContext job;
    p.built = plan(r, caps, job);
    p.finding = validate(r, p.built, answers, ChannelState{});
    if (p.finding.plannable())
        p.ops = lower(p.built.graph, r, job);
    lru_.emplace_front(key, std::move(p));
    entries_[key] = lru_.begin();
    if (entries_.size() > capacity_) {
        entries_.erase(lru_.back().first);
        lru_.pop_back();
    }
    return lru_.front().second;
}

}  // namespace raw::plan
