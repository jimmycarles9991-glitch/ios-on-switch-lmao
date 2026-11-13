// SPDX-License-Identifier: MIT
// What today's encode (raw_gpu.cpp, raw_compute.cpp, RawMetalEncode.mm) emits for the same graph, counted as the planner
// counts: the submission prologue, every pass prologue (RenderEncoder::end), a pass end's tiled drain and storage flush,
// the in-pass fetch and storage barriers with no region test, a WFI at every blit encoder, a compute encoder's opening
// barriers and every serial dispatch's. Its pixel barriers are an upper bound: today a region test on the CPU's reading of
// the vertices drops some.
#pragma once
#include "../plan_barriers.h"

namespace raw::plan::test {

Counts todayCounts(const WorkGraph &g, const Recording &r, bool tiledCache = true);

}  // namespace raw::plan::test
