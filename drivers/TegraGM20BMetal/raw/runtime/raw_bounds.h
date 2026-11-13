// SPDX-License-Identifier: MIT
// The pixels a draw of Core Animation's specialised vertex function can cover, from its vertices on the CPU. QuartzCore's
// fixed_vert_l?h_spc puts each vertex at float4x4 (vertex buffer 2, column-major) x its position (attribute 0, the
// missing components 0, 0, 1 as Metal fills them), then the viewport maps it to window space. Every pixel a triangle
// covers lies in the window-space box of its vertices while every vertex has w > 0: no clip against w = 0 moves a
// projection, and a clip against the view volume only takes pixels away. Positions are worked in double; each carries
// the error the GPU's fp32 transform and subpixel snap may add, and the boxes grow by it.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace raw {

// [x0, x1) x [y0, y1) in pixels
struct PixelBox {
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    bool empty() const
    {
        return x1 <= x0 || y1 <= y0;
    }

    bool overlaps(const PixelBox &o) const
    {
        return x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1;
    }
};

// A Metal viewport: window y grows down from originY while normalized y grows up
struct WindowViewport {
    double originX = 0, originY = 0, width = 0, height = 0;
};

struct WindowPoint {
    double x = 0, y = 0, slack = 0;
    bool unclipped = true;  // z within [0, w]
};

struct PositionStream {
    const uint8_t *base = nullptr;  // vertex 0's bytes
    uint32_t stride = 0, offset = 0;
    uint32_t components = 4;  // MTLVertexFormatFloat2/3/4: 2, 3 or 4 floats
};

class DrawBounds {
public:
    static constexpr uint32_t kMaxCorners = 6;

    DrawBounds(const float matrix[16], const WindowViewport &vp) : vp_(vp)
    {
        memcpy(m_, matrix, sizeof m_);
    }

    // The vertex in window space, and how far the GPU's fp32 transform and subpixel snap may put it from there. False: at
    // or behind w = 0, or not finite
    bool project(const PositionStream &s, uint32_t vertex, WindowPoint &out) const
    {
        float p[4] = {0, 0, 0, 1};
        memcpy(p, s.base + uint64_t(vertex) * s.stride + s.offset, s.components * sizeof(float));
        double clip[4], magnitude[4];
        for (int r = 0; r < 4; r++) {
            clip[r] = magnitude[r] = 0;
            for (int c = 0; c < 4; c++) {
                clip[r] += double(m_[c * 4 + r]) * p[c];
                magnitude[r] += std::fabs(double(m_[c * 4 + r]) * p[c]);
            }
        }
        // the GPU's fp32 component: within kRelative of the sum of its terms' magnitudes, whatever its order or fusing
        const double wError = kRelative * magnitude[3];
        if (!std::isfinite(clip[0]) || !std::isfinite(clip[1]) || !std::isfinite(clip[2]) || !std::isfinite(clip[3]) ||
            clip[3] <= wError)
            return false;
        const double zError = kRelative * magnitude[2];
        out.unclipped = clip[2] - zError >= 0 && clip[2] + zError <= clip[3] - wError;
        out.slack = 0;
        for (int axis = 0; axis < 2; axis++) {
            const double ndc = clip[axis] / clip[3];
            const double ndcError = (kRelative * magnitude[axis] + std::fabs(ndc) * wError) / (clip[3] - wError);
            const double size = axis ? vp_.height : vp_.width;
            const double at = axis ? vp_.originY + (1 - ndc) * 0.5 * size : vp_.originX + (1 + ndc) * 0.5 * size;
            (axis ? out.y : out.x) = at;
            out.slack =
                std::max(out.slack, (ndcError + kRelative) * 0.5 * std::fabs(size) + kRelative * std::fabs(at) + kSnap);
        }
        return true;
    }

    // False: the vertex leaves no box (at or behind w = 0, or not finite), and the draw may cover its whole scissor
    bool add(const PositionStream &s, uint32_t vertex)
    {
        WindowPoint pt;
        if (!project(s, vertex, pt))
            return false;
        addProjected(pt);
        return true;
    }

    // Geometry snapshots project once, then share these points with bounds, coverage and overlap tests.
    void addProjected(const WindowPoint &pt)
    {
        unclipped_ &= pt.unclipped;
        lo_[0] = std::min(lo_[0], pt.x - pt.slack);
        hi_[0] = std::max(hi_[0], pt.x + pt.slack);
        lo_[1] = std::min(lo_[1], pt.y - pt.slack);
        hi_[1] = std::max(hi_[1], pt.y + pt.slack);
        if (count_ < kMaxCorners)
            points_[count_] = pt;
        count_++;
    }

    // The pixels whose centres may fall in the box of every vertex added, and a pixel of guard on each side, within `limit`
    PixelBox pixels(const PixelBox &limit) const
    {
        if (!count_)
            return {};
        return {clampTo(std::ceil(lo_[0] - 0.5) - 1, limit.x0, limit.x1),
                clampTo(std::ceil(lo_[1] - 0.5) - 1, limit.y0, limit.y1),
                clampTo(std::floor(hi_[0] - 0.5) + 2, limit.x0, limit.x1),
                clampTo(std::floor(hi_[1] - 0.5) + 2, limit.y0, limit.y1)};
    }

    // Whether the vertices added are an axis-aligned rectangle's four corners drawn as two triangles split along a
    // diagonal (a 4-vertex triangle strip, or a 6-vertex triangle list: `list`), none clipped by depth. `certain` holds
    // the pixels it covers for sure and `possible` those it may: equal where the corners' errors leave no centre in doubt
    bool rectangle(bool list, PixelBox &certain, PixelBox &possible) const
    {
        if (count_ != (list ? 6u : 4u) || !unclipped_)
            return false;
        double slack = 0, x0 = INFINITY, x1 = -INFINITY, y0 = INFINITY, y1 = -INFINITY;
        for (uint32_t i = 0; i < count_; i++) {
            slack = std::max(slack, points_[i].slack);
            x0 = std::min(x0, points_[i].x);
            x1 = std::max(x1, points_[i].x);
            y0 = std::min(y0, points_[i].y);
            y1 = std::max(y1, points_[i].y);
        }
        const double near = 2 * slack;
        if (x1 - x0 <= 2 * near || y1 - y0 <= 2 * near)
            return false;
        int corner[kMaxCorners];
        for (uint32_t i = 0; i < count_; i++) {
            const WindowPoint &q = points_[i];
            const bool left = std::fabs(q.x - x0) <= near, right = std::fabs(q.x - x1) <= near;
            const bool top = std::fabs(q.y - y0) <= near, bottom = std::fabs(q.y - y1) <= near;
            if (left == right || top == bottom)
                return false;
            corner[i] = (right ? 1 : 0) | (bottom ? 2 : 0);
        }
        if (!(list ? splitList(corner) : splitStrip(corner)))
            return false;
        // a pixel is covered when its centre is inside: for sure past every edge's slack, possibly within it
        certain = {clampTo(std::floor(x0 + slack - 0.5) + 1, 0, ~0u), clampTo(std::floor(y0 + slack - 0.5) + 1, 0, ~0u),
                   clampTo(std::ceil(x1 - slack - 0.5), 0, ~0u), clampTo(std::ceil(y1 - slack - 0.5), 0, ~0u)};
        possible = {clampTo(std::ceil(x0 - slack - 0.5), 0, ~0u), clampTo(std::ceil(y0 - slack - 0.5), 0, ~0u),
                    clampTo(std::floor(x1 + slack - 0.5) + 1, 0, ~0u),
                    clampTo(std::floor(y1 + slack - 0.5) + 1, 0, ~0u)};
        return true;
    }

private:
    // fp32 roundings a vertex's transform, division and viewport mapping may make, and the rasterizer's subpixel snap
    static constexpr double kRelative = 16.0 / (1 << 24), kSnap = 1.0 / 256;

    static uint32_t clampTo(double v, uint32_t lo, uint32_t hi)
    {
        return uint32_t(std::clamp(v, double(lo), double(hi)));
    }

    static bool opposite(int a, int b)
    {
        return (a ^ b) == 3;
    }

    // triangles (0 1 2) and (1 2 3): 1 and 2 on one diagonal, 0 and 3 on the other
    static bool splitStrip(const int c[])
    {
        return opposite(c[1], c[2]) && opposite(c[0], c[3]);
    }

    // triangles (0 1 2) and (3 4 5): each of three distinct corners, the one each leaves out opposite the other's
    static bool splitList(const int c[])
    {
        int missing[2];
        for (int t = 0; t < 2; t++) {
            const int seen = 1 << c[3 * t] | 1 << c[3 * t + 1] | 1 << c[3 * t + 2];
            if (__builtin_popcount(seen) != 3)
                return false;
            missing[t] = __builtin_ctz(~seen & 0xf);
        }
        return opposite(missing[0], missing[1]);
    }

    float m_[16];
    WindowViewport vp_;
    double lo_[2] = {INFINITY, INFINITY}, hi_[2] = {-INFINITY, -INFINITY};
    WindowPoint points_[kMaxCorners];
    uint32_t count_ = 0;
    bool unclipped_ = true;
};

}  // namespace raw
