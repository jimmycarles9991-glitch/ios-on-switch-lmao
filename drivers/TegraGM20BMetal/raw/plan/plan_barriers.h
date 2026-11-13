// SPDX-License-Identifier: MIT
// Places barriers on a built graph: at each node, what its uses need after the earlier nodes' uses of the same memory,
// less what a barrier already placed since then gives; inside a pass, between draw runs, from the colour attachments'
// accesses; at the job's entry, what the CPU and earlier jobs leave; at its exit, the writes nothing has flushed.
#pragma once
#include "plan_hazard.h"

namespace raw::plan {

struct JobContext {
    bool heapPlaced = false;  // the shader heap placed code since this channel last dropped its instruction caches
};

// Methods as kernel/TegraGM20BPushScan.h counts them, plus the drains: points where the 3D pipe empties
struct Counts {
    uint32_t drains = 0, switchDrains = 0;
    uint32_t wfi = 0, shaderInval = 0, shaderInvalNoWfi = 0, texInval = 0, psBarriers = 0;
    uint32_t tiledFlushes = 0, setReference = 0, copyOrdered = 0, headerInval = 0, samplerInval = 0;
};

void addBarriers(Counts &c, Barriers b);
Counts countGraph(const WorkGraph &g);

void placeBarriers(WorkGraph &g, const Recording &r, const Caps &caps, const JobContext &job);

}  // namespace raw::plan
