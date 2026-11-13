// SPDX-License-Identifier: MIT
#pragma once
#include "plan_barriers.h"
#include <vector>

namespace raw::plan {

struct Built {
    WorkGraph graph;
    std::vector<uint32_t> unplanned;  // nodes whose ladder had no selectable rung (validate reports them)
};

// The recording's nodes, implicit work and draw runs, fused passes marked; no barriers yet
Built build(const Recording &r, const Caps &caps);

// build, then barriers placed from the graph's edges
Built plan(const Recording &r, const Caps &caps, const JobContext &job);

}  // namespace raw::plan
