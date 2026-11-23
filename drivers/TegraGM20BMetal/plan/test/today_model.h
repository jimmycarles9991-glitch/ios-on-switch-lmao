// SPDX-License-Identifier: MIT
// What today's encode (gpu.cpp, compute.cpp, TegraGM20BMetalEncode.mm) emits for the same graph, counted as the planner
// counts: the submission prologue, every pass prologue (RenderEncoder::end), a pass end's tiled drain and storage flush,
// the in-pass fetch and storage barriers with no region test, a WFI at every blit encoder, a compute encoder's opening
// barriers and every serial dispatch's. Its pixel barriers are an upper bound: today a region test on the CPU's reading of
// the vertices drops some.
#pragma once
#include "../plan_barriers.h"

namespace gm20b::plan::test {

Counts todayCounts(const WorkGraph &g, const Recording &r, bool tiledCache = true);

}  // namespace gm20b::plan::test
