// SPDX-License-Identifier: MIT
// An app's draw estimated to keep GR longer than TGM_APP_BAND_NS, drawn as the same draw under successive scissor bands.
// GM20B leaves a graphics context only at a wait-for-idle, so a draw in flight always finishes before the compositor's kick
// gets GR. Each band ends a batch (its wait-for-idle seqno release, TegraGM20BPushFence), where the PBDMA stops before the
// next band's methods, so a kick waits for one band at most. The bands are disjoint rows of the draw's scissor area and
// cover it exactly once, so every pixel the draw can write is shaded by exactly one band, in the draw's own primitive
// order: blending, a framebuffer fetch (a pixel reads only itself) and the pixel interlock (a ticket is taken by a
// fragment that is shaded, after the scissor test, and released before the band's end) give what the whole draw gives.
// A visibility report is written between draws, never between a draw's bands, so the sample counter sums the bands as it
// sums the whole draw. A vertex program's side effects would repeat per band: such a draw is drawn whole. Per band the CPU
// writes a scissor and the draw's words.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include "../../../TegraGM20B/TegraGM20BSchedPolicy.h"
#include "raw_bounds.h"
#include "raw_kit.h"
#include "raw_push.h"
#include "raw_rotation.h"

namespace raw {

// GM20B's fragment throughput: 2 SMs of 128 lanes at 768 MHz, one instruction a lane a clock: the static estimate a state's
// draws are priced at until one of them is measured (raw_band_rates.h). Core Animation's full-screen straight-line programs
// measure 8-22% above pixels x instructions / it (narrow_blur_27: 3.66 ms for 670 instructions over 1280x720); the
// instruction count taken from a program's code bytes also counts its scheduling words, one in four, which covers that.
// A program's branches make it an overestimate; its loops, its memory and its ROP traffic an underestimate.
constexpr uint64_t kLaneInstructionsPerUs = 256ull * 768;

// Half a job's internal fences (TegraGM20BMaxJobFences): past them a command buffer splits into more submissions, each a
// kernel call. One draw may take them all.
constexpr uint32_t kBandsPerCommandBuffer = 64;
constexpr uint32_t kMaxBands = kBandsPerCommandBuffer;
constexpr uint32_t kMinBandRows = 16;
// A band's scissor and the longest draw's words (an MME draw: 4), and the scissor put back after the last
constexpr uint32_t kBandWords = 16;

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

// A draw's GPU time: its pixels' (raw_band_rates.h: learned, else the static estimate), and its vertices', which every band
// repeats
struct BandDraw {
    uint64_t fragmentNs = 0, vertexNs = 0;
    uint32_t rows = 0;  // the rows of its pixel box
};

// Into how many bands a draw goes: 1 draws it whole. `left`: the bands its command buffer may still take.
inline uint32_t bandCount(const BandDraw &d, uint64_t budgetNs, uint32_t left)
{
    const uint64_t ns = d.fragmentNs;
    if (!budgetNs || ns <= budgetNs)
        return 1;
    uint64_t n = std::min<uint64_t>({(ns + budgetNs - 1) / budgetNs, kMaxBands, d.rows / kMinBandRows, left});
    while (n > 1 && d.vertexNs * (n - 1) > ns / 8)
        n--;
    return n > 1 ? uint32_t(n) : 1u;
}

// Band i of n: the area's columns and its rows cut evenly over the draw's box, the first band from the area's top and the
// last to its bottom, so the bands cover the area whatever the box holds.
inline PixelBox bandRect(const PixelBox &area, const PixelBox &box, uint32_t i, uint32_t n)
{
    const uint32_t top = std::clamp(box.y0, area.y0, area.y1);
    const uint32_t bottom = std::clamp(box.y1, top, area.y1);
    const auto edge = [&](uint32_t k) {
        if (k == 0)
            return area.y0;
        if (k >= n)
            return area.y1;
        return top + uint32_t(uint64_t(bottom - top) * k / n);
    };
    return {area.x0, edge(i), area.x1, edge(i + 1)};
}

// A frame rectangle as the scissor of the pass's plane (raw_rotation.h)
inline void emitScissor(Push &p, const Rotation &rot, const PixelBox &b)
{
    const Rect s = rot.rect({b.x0, b.y0, b.x1, b.y1});
    kit(p, switch_gm20b_scissor, 0u, uint16_t(s.x0), uint16_t(s.x1), uint16_t(s.y0), uint16_t(s.y1));
}

// The draw in n bands of `area`, its scissor in the frame, over its pixel box: per band the band's scissor, the draw's words
// (words(p)) and the batch's end (cut()), each after room() made kBandWords of room; then `area` again.
template <class Room, class Words, class Cut>
void emitBands(Push &p, const Rotation &rot, const PixelBox &area, const PixelBox &box, uint32_t n, Room &&room,
               Words &&words, Cut &&cut)
{
    for (uint32_t i = 0; i < n; i++) {
        room();
        emitScissor(p, rot, bandRect(area, box, i, n));
        words(p);
        cut();
    }
    room();
    emitScissor(p, rot, area);
}

}  // namespace raw
