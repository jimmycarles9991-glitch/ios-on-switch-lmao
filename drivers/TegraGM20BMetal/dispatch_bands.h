// SPDX-License-Identifier: MIT
// An app's dispatch estimated to keep GR longer than TGM_APP_BAND_NS, launched as consecutive pieces of its threadgroup
// grid, each ending a batch, as a long draw is drawn in bands (draw_bands.h). GM20B's compute does preempt at a
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
#include "draw_bands.h"

namespace gm20b {

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
inline uint32_t pieceCount(uint64_t ns, uint64_t budgetNs, uint32_t axisGroups, uint32_t left,
                           uint32_t maxPieces = kMaxBands, bool unmeasured = false, uint64_t pieceOverheadNs = 0)
{
    if (!budgetNs || ns <= budgetNs)
        return 1;
    if (unmeasured)
        maxPieces = std::min<uint32_t>(maxPieces, kMaxUnmeasuredBands);
    const uint64_t want = (ns + budgetNs - 1) / budgetNs;
    uint64_t cap = std::min<uint64_t>({maxPieces, axisGroups, left});
    const uint64_t perPieceNs = std::max(pieceOverheadNs, std::min<uint64_t>(kBandWfiNs, budgetNs / 10));
    if (perPieceNs) {
        const uint64_t overheadCap = (ns / 8) / perPieceNs + 1;
        cap = std::min(cap, overheadCap);
    }
    const uint64_t n = std::min(want, cap);
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

// What one launch's QMD holds of a grid: CTA_RASTER_WIDTH 31 bits, CTA_RASTER_HEIGHT and CTA_RASTER_DEPTH 16 (QMDV01_07,
// clb1c0qmd.h). A grid past them is launched in slices of at most that many threadgroups an axis, each based where it
// starts as a piece is (cs.base_group), so its threadgroups are the grid's, each once.
constexpr std::array<uint32_t, 3> kQmdGroups = {0x7fffffffu, 0xffffu, 0xffffu};
// The slices of a grid of 2^32 - 1 threadgroups down or deep, the most an axis of a grid gm20b::computeDispatch takes
// needs: a dispatch whose slices would number more is failed, not launched (a root table and a QMD each)
constexpr uint64_t kMaxQmdSlices = (0xffffffffull + 0xfffe) / 0xffff;

inline std::array<uint32_t, 3> qmdSlicesPerAxis(const std::array<uint64_t, 3> &groups)
{
    std::array<uint32_t, 3> n{};
    for (uint8_t a = 0; a < 3; ++a)
        n[a] = groups[a] > kQmdGroups[a] ? uint32_t((groups[a] + kQmdGroups[a] - 1) / kQmdGroups[a]) : 1u;
    return n;
}

inline uint64_t qmdSliceCount(const std::array<uint64_t, 3> &groups)
{
    const std::array<uint32_t, 3> n = qmdSlicesPerAxis(groups);
    return uint64_t(n[0]) * n[1] * n[2];
}

// Slice i (x fastest) of `piece` within the QMD's fields
inline DispatchPiece qmdSlice(const DispatchPiece &piece, uint64_t i)
{
    const std::array<uint32_t, 3> n = qmdSlicesPerAxis({piece.groups[0], piece.groups[1], piece.groups[2]});
    DispatchPiece s;
    for (uint8_t a = 0; a < 3; ++a) {
        const uint32_t k = uint32_t(i % n[a]);
        i /= n[a];
        const uint32_t from = k * kQmdGroups[a];
        s.base[a] = piece.base[a] + from;
        s.groups[a] = std::min(kQmdGroups[a], piece.groups[a] - from);
    }
    return s;
}

}  // namespace gm20b
