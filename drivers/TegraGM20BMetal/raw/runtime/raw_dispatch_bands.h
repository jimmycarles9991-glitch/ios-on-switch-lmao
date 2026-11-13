// SPDX-License-Identifier: MIT
// An app's dispatch estimated to keep GR longer than TGM_APP_BAND_NS, launched as consecutive pieces of its threadgroup
// grid, each ending a batch, as a long draw is drawn in bands (raw_draw_bands.h). GM20B's compute does preempt at a
// threadgroup boundary (nvgpu gr_gm20b.c: NVGPU_PREEMPTION_MODE_COMPUTE_CTA, ctxsw main image word 0x60 = 1), but NVIDIA
// turns it on only for a context whose first object is MAXWELL_COMPUTE_B (gr_gm20b_alloc_gr_ctx); a raw channel's
// context is a 3D one, which leaves at a wait for idle, so a dispatch in flight finishes before a compositor's kick.
//
// A piece is the same launch over a slice of the grid along one axis: NVK compiles every kernel with its workgroup id
// based on the root table's cs.base_group (has_base_workgroup_id; m2v's threadgroup_position_in_grid is WorkgroupId, plus
// the DispatchRegion base for a multi-region one), so a piece's base there and its slice's size in the QMD give the same
// threadgroups the whole launch gives, each once; cs.group_count stays the whole grid's. Metal promises no order or
// concurrency between a dispatch's threadgroups, so pieces run one after another is a schedule the whole dispatch could
// take.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include "raw_draw_bands.h"

namespace raw {

// The axis a dispatch is cut along: its longest
inline uint8_t pieceAxis(const std::array<uint32_t, 3> &groups)
{
    uint8_t axis = 0;
    for (uint8_t i = 1; i < 3; i++)
        if (groups[i] > groups[axis])
            axis = i;
    return axis;
}

// Into how many pieces a dispatch of `ns` goes: 1 launches it whole. `left`: the bands its command buffer may still take.
inline uint32_t pieceCount(uint64_t ns, uint64_t budgetNs, uint32_t axisGroups, uint32_t left)
{
    if (!budgetNs || ns <= budgetNs)
        return 1;
    const uint64_t n = std::min<uint64_t>({(ns + budgetNs - 1) / budgetNs, kMaxBands, axisGroups, left});
    return n > 1 ? uint32_t(n) : 1u;
}

struct DispatchPiece {
    std::array<uint32_t, 3> base{}, groups{};
};

// Piece i of n of a grid of `groups` threadgroups along `axis`: the slices are disjoint and cover the grid
inline DispatchPiece dispatchPiece(const std::array<uint32_t, 3> &groups, uint8_t axis, uint32_t i, uint32_t n)
{
    DispatchPiece p;
    p.groups = groups;
    const uint32_t from = uint32_t(uint64_t(groups[axis]) * i / n), to = uint32_t(uint64_t(groups[axis]) * (i + 1) / n);
    p.base[axis] = from;
    p.groups[axis] = to - from;
    return p;
}

}  // namespace raw
