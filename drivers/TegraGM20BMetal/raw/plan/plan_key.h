// SPDX-License-Identifier: MIT
// The structural key of a recording: everything a planner decision reads, nothing it does not. Resources enter by their
// recording slot (first use) and the pattern of memory they share, so a frame on another drawable of the same shape keys
// alike; draw counts, offsets, clear values and event values stay out, being lowering parameters.
#pragma once
#include "plan_caps.h"

namespace raw::plan {

uint64_t structuralKey(const Recording &r, const Caps &caps);

// Each node's key for a fragment cache: what its words are made from, its barriers included; draw counts per run stay in
// (a run's draws are words), their arguments do not
void nodeKeys(WorkGraph &g, const Recording &r);

}  // namespace raw::plan
