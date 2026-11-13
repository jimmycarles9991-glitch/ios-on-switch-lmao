// SPDX-License-Identifier: MIT
// Plans by structural key, least recently used out first: a frame whose structure repeats costs the key and a lookup.
// One cache per queue; nothing in it is shared, so it takes no lock.
#pragma once
#include "plan_lower.h"
#include "plan_validate.h"
#include <cstddef>
#include <list>
#include <unordered_map>

namespace raw::plan {

struct Plan {
    Built built;
    Finding finding;
    std::vector<EmitOp> ops;  // lowered with no instruction-cache invalidate; the job adds it (JobContext)
};

class PlanCache {
public:
    explicit PlanCache(size_t capacity = 32) : capacity_(capacity)
    {}

    // The plan of this recording's structure, planned and validated the first time the structure is seen
    const Plan &get(const Recording &r, const Caps &caps, const Answers &answers);

    uint64_t hits() const
    {
        return hits_;
    }

    uint64_t plans() const
    {
        return plans_;
    }

    size_t size() const
    {
        return entries_.size();
    }

private:
    using Entry = std::pair<uint64_t, Plan>;

    size_t capacity_;
    std::list<Entry> lru_;
    std::unordered_map<uint64_t, std::list<Entry>::iterator> entries_;
    uint64_t hits_ = 0, plans_ = 0;
};

}  // namespace raw::plan
