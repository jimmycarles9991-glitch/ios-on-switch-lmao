// SPDX-License-Identifier: MIT
// An app's draw estimated to keep GR longer than TGM_APP_BAND_NS, drawn as the same draw under successive scissor
// bands. GM20B leaves a graphics context only at a wait-for-idle, so a draw in flight always finishes before the
// compositor's kick gets GR. Each band ends in a wait-for-idle semaphore release: into the channel's progress stamp
// where it has one (every app channel does, device_channel.cpp), else a batch end's seqno release
// (TegraGM20BPushFence). GR is idle there, so a kick's preempt can land between bands. The bands are disjoint rows of
// the draw's scissor area and cover it exactly once, so every pixel the draw can write is shaded by exactly one band,
// in the draw's own primitive order: blending, a framebuffer fetch (a pixel reads only itself) and the pixel interlock
// (a ticket is taken by a fragment that is shaded, after the scissor test, and released before the band's end) give
// what the whole draw gives. A visibility report is written between draws, never between a draw's bands, so the sample
// counter sums the bands as it sums the whole draw. A vertex program's side effects would repeat per band: such a draw
// is drawn whole. Per band the CPU writes a scissor and the draw's words.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include "../../kits/SwitchGM20B/SwitchGM20B.h"
#include "../TegraGM20B/TegraGM20BSchedPolicy.h"
#include "bounds.h"
#include "kit.h"
#include "push.h"
#include "rotation.h"

namespace gm20b {

// GM20B's fragment throughput: 2 SMs of 128 lanes at 768 MHz, one instruction a lane a clock: the static estimate a state's
// draws are priced at until one of them is measured (band_rates.h). Core Animation's full-screen straight-line programs
// measure 8-22% above pixels x instructions / it (narrow_blur_27: 3.66 ms for 670 instructions over 1280x720); the
// instruction count taken from a program's code bytes also counts its scheduling words, one in four, which covers that.
// A program's branches make it an overestimate; its loops, its memory and its ROP traffic an underestimate.
constexpr uint64_t kLaneInstructionsPerUs = 256ull * 768;

// Half a job's internal fences (TegraGM20BMaxJobFences): legacy per-buffer cap when no progress word is available.
constexpr uint32_t kBandsPerCommandBuffer = 64;
constexpr uint32_t kMaxBands = kBandsPerCommandBuffer;
constexpr uint32_t kMaxTileBands = 4096;
constexpr uint32_t kMaxUnmeasuredBands = 16;
// Branching uber-shaders (e.g. RenderBox 15K+ instr) execute one branch per pixel; cap unmeasured static pricing until measured.
constexpr uint32_t kMaxStaticInstructions = 4096;
// Floor per-band WFI drain and relaunch cost on GM20B so banding overhead stays bounded before nsPerBand is learned.
constexpr uint64_t kBandWfiNs = 80'000;
constexpr uint32_t kMinBandRows = 16;
constexpr uint32_t kMinBandCols = 16;
// A band's scissor, draw words and progress release, plus the scissor put back after the last
constexpr uint32_t kBandWords = 24;
constexpr uint32_t kProgressReleaseWords = 5;

constexpr uint64_t kWorkMax = UINT64_MAX / 1000;

// a x b lane instructions, at most kWorkMax (BatchWork's own product stops at a batch's limit)
inline uint64_t laneWork(uint64_t a, uint64_t b)
{
    return b && a > kWorkMax / b ? kWorkMax : a * b;
}

inline uint64_t workNs(uint64_t laneInstructions)
{
    return std::min(laneInstructions, kWorkMax) * 1000 / kLaneInstructionsPerUs;
}

// Whether a program writes memory: its shader header's DOES_GLOBAL_STORE (word 0 bit 16), which NAK sets for any global
// store, atomic or surface write. None: as if it did.
inline bool writesMemory(const uint8_t *header)
{
    uint32_t word0 = 0;
    if (!header)
        return true;
    memcpy(&word0, header, sizeof word0);
    return (word0 >> 16 & 1) != 0;
}

// Host WFI semaphore release into the channel's progress stamp (no interrupt): gives the host a preempt point and the
// kernel's hang rule visible progress (the payload word). Only debug kernels read the PTIMER stamp a 16-byte release
// adds (hold_stats.c, mid-job preempt landings), so builds without diagnostics write the payload alone.
inline void emitProgressRelease(Push &p, uint64_t va, uint32_t payload)
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    constexpr uint32_t kSize = NVDEF(NVB06F, SEMAPHORED, RELEASE_SIZE, 16BYTE);
#else
    constexpr uint32_t kSize = NVDEF(NVB06F, SEMAPHORED, RELEASE_SIZE, 4BYTE);
#endif
    p.inc(NVB06F_SEMAPHOREA,
          {uint32_t(va >> 32) & 0xff, uint32_t(va), payload,
           NVDEF(NVB06F, SEMAPHORED, OPERATION, RELEASE) | NVDEF(NVB06F, SEMAPHORED, RELEASE_WFI, EN) | kSize});
}

// A draw's GPU time: its pixels' (band_rates.h: learned, else the static estimate), and its vertices', which every band
// repeats
struct BandDraw {
    uint64_t fragmentNs = 0, vertexNs = 0;
    uint32_t rows = 0, cols = 0;
    bool unmeasured = false;
};

struct BandGrid {
    uint32_t rows = 1, cols = 1;

    uint32_t total() const
    {
        return rows * cols;
    }
};

inline BandGrid bandGridLayout(uint64_t want, uint32_t maxRows, uint32_t maxCols, uint64_t cap)
{
    want = std::min(want, cap);
    if (want <= 1)
        return {};
    if (want <= maxRows)
        return {uint32_t(want), 1u};
    uint32_t cols = uint32_t(std::min<uint64_t>(maxCols, (want + maxRows - 1) / maxRows));
    uint32_t rows = uint32_t(std::min<uint64_t>(maxRows, (want + cols - 1) / cols));
    if (uint64_t(rows) * cols > cap) {
        rows = uint32_t(cap / cols);
        if (rows == 0) {
            rows = 1;
            cols = uint32_t(std::min<uint64_t>(maxCols, cap));
        }
    }
    return rows * cols > 1 ? BandGrid{rows, cols} : BandGrid{};
}

// Into how many row-by-column tiles a draw goes: {1, 1} draws it whole. Cuts rows first, then splits columns once
// rows hit kMinBandRows. Unmeasured draws stay whole when priced within budget and cap at kMaxUnmeasuredBands until measured.
inline BandGrid bandGrid(const BandDraw &d, uint64_t budgetNs, uint32_t left, uint32_t maxBands = kMaxBands)
{
    if (!budgetNs || !d.rows || left <= 1 || maxBands <= 1)
        return {};
    const uint64_t ns = d.fragmentNs;
    if (ns <= budgetNs)
        return {};
    const uint32_t maxRows = std::max<uint32_t>(d.rows / kMinBandRows, 1u);
    const uint32_t maxCols = d.cols >= kMinBandCols ? d.cols / kMinBandCols : 1u;
    const uint64_t maxGeom = uint64_t(maxRows) * maxCols;
    if (maxGeom <= 1)
        return {};
    if (d.unmeasured)
        maxBands = std::min<uint32_t>(maxBands, kMaxUnmeasuredBands);
    const uint64_t want = (ns + budgetNs - 1) / budgetNs;
    uint64_t cap = std::min<uint64_t>({maxGeom, maxBands, left});
    const uint64_t perBandNs = std::max(d.vertexNs, std::min<uint64_t>(kBandWfiNs, budgetNs / 10));
    if (perBandNs) {
        const uint64_t overheadCap = (ns / 8) / perBandNs + 1;
        cap = std::min(cap, overheadCap);
    }
    if (cap <= 1)
        return {};
    return bandGridLayout(want, maxRows, maxCols, cap);
}

inline BandGrid bandGridFor(const BandDraw &d, uint32_t want, uint32_t left, uint32_t maxBands = kMaxBands)
{
    if (want <= 1 || !d.rows || left <= 1 || maxBands <= 1)
        return {};
    const uint32_t maxRows = std::max<uint32_t>(d.rows / kMinBandRows, 1u);
    const uint32_t maxCols = d.cols >= kMinBandCols ? d.cols / kMinBandCols : 1u;
    const uint64_t cap = std::min<uint64_t>({uint64_t(maxRows) * maxCols, maxBands, left});
    return bandGridLayout(want, maxRows, maxCols, cap);
}

// Into how many bands a draw goes: 1 draws it whole. `left`: the bands its command buffer may still take.
inline uint32_t bandCount(const BandDraw &d, uint64_t budgetNs, uint32_t left)
{
    return bandGrid(d, budgetNs, left, kMaxBands).total();
}

// Tile (col, row) of cols x rows: the area's columns and rows cut evenly over the draw's box, outer edges extended to
// `area` so the tiles partition `area` once whatever `box` holds.
inline PixelBox bandTileRect(const PixelBox &area, const PixelBox &box, uint32_t col, uint32_t cols, uint32_t row,
                             uint32_t rows)
{
    const uint32_t left = std::clamp(box.x0, area.x0, area.x1);
    const uint32_t right = std::clamp(box.x1, left, area.x1);
    const uint32_t top = std::clamp(box.y0, area.y0, area.y1);
    const uint32_t bottom = std::clamp(box.y1, top, area.y1);
    const auto xEdge = [&](uint32_t k) {
        if (k == 0)
            return area.x0;
        if (k >= cols)
            return area.x1;
        return left + uint32_t(uint64_t(right - left) * k / cols);
    };
    const auto yEdge = [&](uint32_t k) {
        if (k == 0)
            return area.y0;
        if (k >= rows)
            return area.y1;
        return top + uint32_t(uint64_t(bottom - top) * k / rows);
    };
    return {xEdge(col), yEdge(row), xEdge(col + 1), yEdge(row + 1)};
}

// Band i of n: the area's columns and its rows cut evenly over the draw's box, the first band from the area's top and the
// last to its bottom, so the bands cover the area whatever the box holds.
inline PixelBox bandRect(const PixelBox &area, const PixelBox &box, uint32_t i, uint32_t n)
{
    return bandTileRect(area, box, 0, 1, i, n);
}

// A frame rectangle as the scissor of the pass's plane (rotation.h)
inline void emitScissor(Push &p, const Rotation &rot, const PixelBox &b)
{
    const Rect s = rot.rect({b.x0, b.y0, b.x1, b.y1});
    kit(p, switch_gm20b_scissor, 0u, uint16_t(s.x0), uint16_t(s.x1), uint16_t(s.y0), uint16_t(s.y1));
}

template <class Room, class Words, class Cut>
void emitBandGrid(Push &p, const Rotation &rot, const PixelBox &area, const PixelBox &box, BandGrid grid, Room &&room,
                  Words &&words, Cut &&cut)
{
    for (uint32_t r = 0; r < grid.rows; r++)
        for (uint32_t c = 0; c < grid.cols; c++) {
            room();
            emitScissor(p, rot, bandTileRect(area, box, c, grid.cols, r, grid.rows));
            words(p);
            cut();
        }
    room();
    emitScissor(p, rot, area);
}

// The draw in n bands of `area`, its scissor in the frame, over its pixel box: per band the band's scissor, the draw's words
// (words(p)) and the batch's end (cut()), each after room() made kBandWords of room; then `area` again.
template <class Room, class Words, class Cut>
void emitBands(Push &p, const Rotation &rot, const PixelBox &area, const PixelBox &box, uint32_t n, Room &&room,
               Words &&words, Cut &&cut)
{
    emitBandGrid(p, rot, area, box, {n, 1u}, std::forward<Room>(room), std::forward<Words>(words),
                 std::forward<Cut>(cut));
}

}  // namespace gm20b
