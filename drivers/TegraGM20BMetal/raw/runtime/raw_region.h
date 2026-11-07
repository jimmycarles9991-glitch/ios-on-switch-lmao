// SPDX-License-Identifier: MIT
// The fetch barrier's region test: whether a fetching draw's pixels can meet those a draw since the last barrier wrote.
// Each draw keeps what its vertices need (raw_bounds.h) and the CPU reads them only when a fetching draw asks, newest
// first, until one meets it. A draw whose pixels it cannot bound (not Core Animation's transform, its matrix or vertices
// not readable here, too many vertices, a vertex at or behind w = 0) may cover its whole scissor.
//
// Skipping the barrier where no written pixel is fetched is exact on uncompressed (pitch and generic block-linear)
// targets: a texel's bytes change only where the ROP writes that pixel, so a fetch of any other pixel reads the same
// bytes whether or not the earlier writes have landed, from the texture cache or from L2.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>

#include "raw_bounds.h"

namespace raw {

struct DrawGeometry {
    bool bounded = false;  // Core Animation's transform over triangles, its inputs where the CPU can read them
    const uint8_t *matrix = nullptr;  // the float4x4 at vertex buffer 2
    PositionStream positions;
    uint64_t positionBytes = 0;        // bound from positions.base
    uint32_t first = 0, count = 0;     // vertices, or indices where `indices` is set
    const uint8_t *indices = nullptr;  // 2- or 4-byte indices
    uint32_t indexSize = 0;
    WindowViewport viewport;
    PixelBox limit;  // its scissor within its viewport within the render area
};

class WrittenRegion {
public:
    static constexpr uint32_t kMaxDraws = 16, kMaxVertices = 96;

    void clear()
    {
        count_ = 0;
        overflow_ = false;
    }

    void add(const DrawGeometry &g)
    {
        if (g.limit.empty())
            return;
        if (count_ == kMaxDraws) {
            overflow_ = true;
            return;
        }
        draws_[count_++] = {g, g.limit, !g.bounded};
    }

    // Whether `g`'s pixels may meet a pixel written since the last clear
    bool meets(const DrawGeometry &g)
    {
        if (g.limit.empty())
            return false;
        if (overflow_)
            return true;
        PixelBox box;
        if (!bounds(g, box))
            box = g.limit;
        for (uint32_t i = count_; i-- > 0;) {
            Draw &d = draws_[i];
            if (!d.known) {
                d.known = true;
                if (!bounds(d.geometry, d.box))
                    d.box = d.geometry.limit;
            }
            if (d.box.overlaps(box))
                return true;
        }
        return false;
    }

    static bool bounds(const DrawGeometry &g, PixelBox &out)
    {
        if (!g.bounded || g.count > kMaxVertices)
            return false;
        float matrix[16];
        memcpy(matrix, g.matrix, sizeof matrix);
        DrawBounds b(matrix, g.viewport);
        uint32_t added = 0;
        if (!addVertices(g, b, added))
            return false;
        out = b.pixels(g.limit);
        return true;
    }

    // Whether the draw covers every pixel of [0, w) x [0, h) for sure: all its vertices one axis-aligned rectangle (a
    // triangle `list` of 6, or a strip of 4 with no restart), within a scissor and viewport that hold the whole area
    static bool covers(const DrawGeometry &g, bool list, uint32_t w, uint32_t h)
    {
        if (!g.bounded || g.count != (list ? 6u : 4u) || g.limit.x0 || g.limit.y0 || g.limit.x1 < w || g.limit.y1 < h)
            return false;
        float matrix[16];
        memcpy(matrix, g.matrix, sizeof matrix);
        DrawBounds b(matrix, g.viewport);
        uint32_t added = 0;
        PixelBox certain, possible;
        return addVertices(g, b, added) && added == g.count && b.rectangle(list, certain, possible) && !certain.x0 &&
               !certain.y0 && certain.x1 >= w && certain.y1 >= h;
    }

    // Every vertex of `g` into `b` but a strip's restarts (`added` counts those taken); false: one past the bound bytes,
    // or one that leaves no box (raw_bounds.h add)
    static bool addVertices(const DrawGeometry &g, DrawBounds &b, uint32_t &added)
    {
        const uint64_t reach = g.positions.offset + g.positions.components * sizeof(float);
        for (uint32_t i = 0; i < g.count; i++) {
            uint32_t v = g.first + i;
            if (g.indices) {
                if (g.indexSize == 2) {
                    uint16_t x;
                    memcpy(&x, g.indices + 2 * i, 2);
                    v = x == 0xffff ? ~0u : x;
                } else {
                    memcpy(&v, g.indices + 4 * i, 4);
                }
                if (v == ~0u)
                    continue;  // a strip's restart
            }
            if (uint64_t(v) * g.positions.stride + reach > g.positionBytes || !b.add(g.positions, v))
                return false;
            added++;
        }
        return true;
    }

private:
    struct Draw {
        DrawGeometry geometry;
        PixelBox box;
        bool known;
    };

    std::array<Draw, kMaxDraws> draws_;
    uint32_t count_ = 0;
    bool overflow_ = false;
};

}  // namespace raw
