// SPDX-License-Identifier: MIT
// A framebuffer-fetching draw that writes a color it fetches must see, at each pixel, what its own earlier triangles wrote
// there (Metal's raster order). The fetch barrier orders draws, not the triangles of one draw, so such a draw is drawn as
// runs of consecutive triangles no two of which can cover the same pixel, with the barrier between runs.
//
// Two triangles cover no pixel centre in common when a line through an edge of one has the other on its far side, every
// vertex strictly but those exactly on the line. Maxwell's raster unit covers a pixel whose centre is inside the triangle
// or on one of its top or left edges (the top-left rule of D3D10 and later, which it implements whatever the API; NVK
// sets SET_WINDOW_ORIGIN upper left): a centre on a line is covered only by a triangle whose interior lies right of or
// below it there, so triangles on either side of a line never cover the same centre, along it or off it. A vertex is
// exactly on the line where the GPU computes its window position from the same inputs as the edge's ends: it is one of
// them (equal position bytes), or the edge is vertical (horizontal) because its ends share x (y), z and w, which alone
// make window x (y) when the matrix takes nothing of y (x) into x (y) or w, and the vertex shares them too. A triangle
// with two equal vertices has no area and covers nothing.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "raw_region.h"

namespace raw {

class TriangleRuns {
public:
    static constexpr uint32_t kMaxTriangles = 96;

    // [begin, end) element ranges (vertices from the draw's first, or index positions) of the runs, in order. False: the
    // draw cannot be told apart into triangles here (not bounded, too many, a vertex it cannot place), and stays whole
    bool split(const DrawGeometry &g, bool strip, std::vector<std::pair<uint32_t, uint32_t>> &runs)
    {
        runs.clear();
        if (!g.bounded || !gather(g, strip))
            return false;
        uint32_t runFirst = 0;
        for (uint32_t t = 1; t < count_; t++) {
            if (tri_[t].empty)
                continue;
            for (uint32_t u = runFirst; u < t; u++) {
                if (!tri_[u].empty && mayShare(tri_[u], tri_[t])) {
                    runs.push_back({runs.empty() ? 0 : tri_[runFirst].element, tri_[t - 1].element + 3});
                    runFirst = t;
                    break;
                }
            }
        }
        if (!runs.empty())
            runs.push_back({tri_[runFirst].element, g.count});
        return true;
    }

private:
    struct Vertex {
        WindowPoint at;
        uint32_t bits[4];
    };

    // the matrix takes nothing of y into x and w (exact vertical lines), of x into y and w (exact horizontal ones)
    bool exactX_ = false, exactY_ = false;

    struct Triangle {
        Vertex v[3];
        uint32_t element;  // its first vertex's element
        bool empty;        // two of its vertices are one: no area
        double x0, y0, x1, y1;
    };

    bool gather(const DrawGeometry &g, bool strip)
    {
        count_ = 0;
        float matrix[16];
        memcpy(matrix, g.matrix, sizeof matrix);
        exactX_ = matrix[4] == 0 && matrix[7] == 0;
        exactY_ = matrix[1] == 0 && matrix[3] == 0;
        const DrawBounds bounds(matrix, g.viewport);
        const uint64_t reach = g.positions.offset + g.positions.components * sizeof(float);
        Vertex window[3];
        uint32_t first = 0, have = 0;  // the element of window[0], and how many of window hold this strip's vertices
        for (uint32_t e = 0; e < g.count; e++) {
            uint32_t v = g.first + e;
            if (g.indices) {
                uint32_t x;
                if (g.indexSize == 2) {
                    uint16_t h;
                    memcpy(&h, g.indices + 2 * e, 2);
                    x = strip && h == 0xffff ? ~0u : h;
                } else {
                    memcpy(&x, g.indices + 4 * e, 4);
                }
                if (strip && x == ~0u) {
                    have = 0;  // a restart begins a new strip
                    continue;
                }
                v = g.first + x;
            }
            if (have == 3) {
                window[0] = window[1];
                window[1] = window[2];
                first++;
                have = 2;
            }
            if (!have)
                first = e;
            Vertex &w = window[have++];
            if (uint64_t(v) * g.positions.stride + reach > g.positionBytes || !bounds.project(g.positions, v, w.at))
                return false;
            memset(w.bits, 0, sizeof w.bits);
            memcpy(w.bits, g.positions.base + uint64_t(v) * g.positions.stride + g.positions.offset,
                   g.positions.components * sizeof(float));
            if (have < 3)
                continue;
            if (count_ == kMaxTriangles)
                return false;
            add(window, first);
            if (!strip)
                have = 0;
        }
        return true;
    }

    void add(const Vertex (&v)[3], uint32_t element)
    {
        Triangle &t = tri_[count_++];
        t.element = element;
        t.empty = same(v[0], v[1]) || same(v[1], v[2]) || same(v[0], v[2]);
        t.x0 = t.y0 = INFINITY;
        t.x1 = t.y1 = -INFINITY;
        for (int i = 0; i < 3; i++) {
            t.v[i] = v[i];
            t.x0 = std::min(t.x0, v[i].at.x - v[i].at.slack);
            t.x1 = std::max(t.x1, v[i].at.x + v[i].at.slack);
            t.y0 = std::min(t.y0, v[i].at.y - v[i].at.slack);
            t.y1 = std::max(t.y1, v[i].at.y + v[i].at.slack);
        }
    }

    static bool same(const Vertex &a, const Vertex &b)
    {
        return !memcmp(a.bits, b.bits, sizeof a.bits);
    }

    static bool share(const Vertex &a, const Vertex &b, int axis)
    {
        return a.bits[axis] == b.bits[axis] && a.bits[2] == b.bits[2] && a.bits[3] == b.bits[3];
    }

    // whether the GPU puts v exactly on the line through p and q
    bool onLine(const Vertex &v, const Vertex &p, const Vertex &q) const
    {
        if (same(v, p) || same(v, q))
            return true;
        for (int axis = 0; axis < 2; axis++)
            if ((axis ? exactY_ : exactX_) && share(p, q, axis) && share(v, p, axis))
                return true;
        return false;
    }

    bool mayShare(const Triangle &a, const Triangle &b) const
    {
        // a pixel's centre covered by both lies in both boxes, half a pixel in from each pixel's edge
        if (a.x1 <= b.x0 || b.x1 <= a.x0 || a.y1 <= b.y0 || b.y1 <= a.y0)
            return false;
        for (int i = 0; i < 3; i++)
            if (separates(a, i, b) || separates(b, i, a))
                return false;
        return true;
    }

    // Whether the line through edge `i` of `a` (its vertices i and i + 1) has `b` on the far side from `a`'s third vertex
    bool separates(const Triangle &a, int i, const Triangle &b) const
    {
        const Vertex &p = a.v[i], &q = a.v[(i + 1) % 3], &r = a.v[(i + 2) % 3];
        const double dx = q.at.x - p.at.x, dy = q.at.y - p.at.y, length = std::hypot(dx, dy);
        if (!(length > 0))
            return false;
        auto side = [&](const Vertex &v) {
            return (dx * (v.at.y - p.at.y) - dy * (v.at.x - p.at.x)) / length;
        };
        auto error = [&](const Vertex &v) {
            const double along = std::hypot(v.at.x - p.at.x, v.at.y - p.at.y) / length;
            return v.at.slack + p.at.slack + q.at.slack + along * (p.at.slack + q.at.slack);
        };
        const double own = side(r);
        if (std::fabs(own) <= error(r))
            return false;
        for (const Vertex &v : b.v) {
            if (onLine(v, p, q))
                continue;
            if ((own > 0 ? side(v) : -side(v)) >= -error(v))
                return false;
        }
        return true;
    }

    Triangle tri_[kMaxTriangles];
    uint32_t count_ = 0;
};

}  // namespace raw
