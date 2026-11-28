// SPDX-License-Identifier: MIT
// The fetch barrier's region test: whether a fetching draw's pixels can meet those a draw since the last barrier wrote.
// Bounded Core Animation draws (setBytes vertices + fixed transform) resolve their projected window-space AABBs lazily
// when a fetching draw queries meets(), falling back conservatively to the scissor/viewport limit for unbounded,
// over-budget, or overflowed pending draws. A 64x64 bitmask of 32x32 pixel tiles tracks writes past the exact-box table.
//
// Skipping the barrier where no written pixel is fetched is exact on uncompressed (pitch and generic block-linear)
// targets: a texel's bytes change only where the ROP writes that pixel, so a fetch of any other pixel reads the same
// bytes whether or not the earlier writes have landed, from the texture cache or from L2. Compressed targets share
// per-tile compression metadata, so elision requires disjoint 32x32 pixel tiles (meetsTiles), which align to GM20B
// C32/C64/C128 compression tiles across 1x/2x/4x MSAA.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include "geometry.h"

namespace gm20b {

class WrittenRegion {
public:
    static constexpr uint32_t kMaxDraws = 64, kMaxVertices = GeometrySnapshot::kMaxVertices;
    static constexpr uint32_t kTileShift = 5, kTileGrid = 64, kGridMax = kTileGrid << kTileShift;
    static constexpr uint32_t kMaxPending = 256;
    using Analysis = GeometryAnalysis;

    void clear()
    {
        pending_.clear();
        overflowLimit_ = {};
        boxCount_ = 0;
        if (uint64_t rows = dirtyTileRows_) {
            for (; rows; rows &= rows - 1)
                tiles_[uint32_t(__builtin_ctzll(rows))] = 0;
            dirtyTileRows_ = 0;
        }
        tileOnly_ = false;
        outOfGrid_ = false;
        outOfGridBox_ = {};
        lastNonOverlappingQuad_ = false;
    }

    // the list, its capacity for another region (the render encoder's next pass: no allocation a steady frame)
    std::vector<Analysis> release()
    {
        clear();
        return std::move(pending_);
    }

    void adopt(std::vector<Analysis> &&storage)
    {
        pending_ = std::move(storage);
        clear();
    }

    void setLastNonOverlappingQuad(bool value)
    {
        lastNonOverlappingQuad_ = value;
    }

    bool lastNonOverlappingQuad() const
    {
        return lastNonOverlappingQuad_;
    }

    void add(const DrawGeometry &g)
    {
        Analysis analysis(g);
        add(analysis);
    }

    void add(const Analysis &analysis)
    {
        const PixelBox &limit = analysis.geometry().limit;
        if (limit.empty())
            return;
        if (analysis.evaluated() || !analysis.geometry().bounded) {
            recordBox(analysis.cachedBox());
            return;
        }
        if (pending_.size() == kMaxPending) {
            if (overflowLimit_.empty()) {
                overflowLimit_ = limit;
            } else {
                overflowLimit_.x0 = std::min(overflowLimit_.x0, limit.x0);
                overflowLimit_.y0 = std::min(overflowLimit_.y0, limit.y0);
                overflowLimit_.x1 = std::max(overflowLimit_.x1, limit.x1);
                overflowLimit_.y1 = std::max(overflowLimit_.y1, limit.y1);
            }
            return;
        }
        pending_.push_back(analysis);
    }

    // Whether `g`'s pixels may meet a pixel written since the last clear
    bool meets(const DrawGeometry &g)
    {
        Analysis analysis(g);
        return meets(analysis);
    }

    bool meets(Analysis &analysis)
    {
        const DrawGeometry &g = analysis.geometry();
        if (g.limit.empty())
            return false;
        if (!boxCount_ && pending_.empty() && overflowLimit_.empty() && !tileOnly_ && !outOfGrid_)
            return false;
        materializePending();
        PixelBox box;
        if (g.bounded)
            box = analysis.bounds(box) ? box : g.limit;
        else
            box = g.limit;
        if (box.empty())
            return false;
        const bool tileHit = tilesOverlap(box);
        const bool outsideHit = outOfGrid_ && outOfGridBox_.overlaps(box);
        if (!tileHit && !outsideHit)
            return false;
        if (tileOnly_)
            return true;
        for (uint32_t i = boxCount_; i-- > 0;)
            if (boxes_[i].overlaps(box))
                return true;
        return false;
    }

    bool meetsTiles(const DrawGeometry &g)
    {
        Analysis analysis(g);
        return meetsTiles(analysis);
    }

    bool meetsTiles(Analysis &analysis)
    {
        const DrawGeometry &g = analysis.geometry();
        if (g.limit.empty())
            return false;
        if (!boxCount_ && pending_.empty() && overflowLimit_.empty() && !tileOnly_ && !outOfGrid_)
            return false;
        materializePending();
        PixelBox box;
        if (g.bounded)
            box = analysis.bounds(box) ? box : g.limit;
        else
            box = g.limit;
        if (box.empty())
            return false;
        if (tilesOverlap(box))
            return true;
        if (!outOfGrid_)
            return false;
        if (tileOnly_)
            return boxesShareTile(outOfGridBox_, box);
        for (uint32_t i = boxCount_; i-- > 0;)
            if ((boxes_[i].x1 > kGridMax || boxes_[i].y1 > kGridMax) && boxesShareTile(boxes_[i], box))
                return true;
        return false;
    }

    // Whether a 2-triangle draw (4-vertex strip or 6-vertex list sharing an exact edge) has strictly disjoint triangle
    // interiors on opposite sides of the shared diagonal under fp32 and subpixel snap slack, inside the X/Y guard band.
    static bool nonOverlappingQuad(Analysis &analysis, bool list)
    {
        const DrawGeometry &g = analysis.geometry();
        const uint32_t expected = list ? 6u : 4u;
        if (!g.bounded || g.count != expected)
            return false;
        PixelBox box;
        if (!analysis.bounds(box))
            return false;
        const GeometrySnapshot &snap = analysis.snapshot();
        const uint32_t base = analysis.begin();
        if (base + expected > snap.points.size())
            return false;
        for (uint32_t i = 0; i < expected; ++i)
            if (snap.points[base + i].restart)
                return false;

        WindowPoint u, v, a, b;
        if (!list) {
            u = snap.points[base + 1].at;
            v = snap.points[base + 2].at;
            a = snap.points[base + 0].at;
            b = snap.points[base + 3].at;
        } else {
            const size_t cmpBytes = size_t(g.positions.components) * sizeof(float);
            const auto sameVertex = [&](uint32_t i, uint32_t j) {
                return memcmp(snap.points[base + i].bits, snap.points[base + j].bits, cmpBytes) == 0;
            };
            bool used1[3] = {};
            uint32_t shared[2] = {};
            uint32_t sharedCount = 0;
            uint32_t aIdx = 3;
            for (uint32_t i = 0; i < 3; ++i) {
                bool matched = false;
                for (uint32_t j = 0; j < 3; ++j) {
                    if (!used1[j] && sameVertex(i, 3 + j)) {
                        used1[j] = true;
                        matched = true;
                        if (sharedCount < 2)
                            shared[sharedCount] = i;
                        ++sharedCount;
                        break;
                    }
                }
                if (!matched)
                    aIdx = i;
            }
            if (sharedCount != 2 || aIdx >= 3)
                return false;
            uint32_t bIdx = 6;
            for (uint32_t j = 0; j < 3; ++j)
                if (!used1[j])
                    bIdx = 3 + j;
            if (bIdx >= 6)
                return false;
            u = snap.points[base + shared[0]].at;
            v = snap.points[base + shared[1]].at;
            a = snap.points[base + aIdx].at;
            b = snap.points[base + bIdx].at;
        }
        const double cx = g.viewport.originX + 0.5 * g.viewport.width;
        const double cy = g.viewport.originY + 0.5 * g.viewport.height;
        const double gx = std::min(128.0 * std::fabs(g.viewport.width), 16384.0);
        const double gy = std::min(128.0 * std::fabs(g.viewport.height), 16384.0);
        const auto inGuardBand = [&](const WindowPoint &p) {
            return p.unclipped && std::fabs(p.x - cx) + p.slack < gx && std::fabs(p.y - cy) + p.slack < gy &&
                   std::fabs(p.x) + p.slack < 16384.0 && std::fabs(p.y) + p.slack < 16384.0;
        };
        if (!inGuardBand(u) || !inGuardBand(v) || !inGuardBand(a) || !inGuardBand(b))
            return false;

        const auto signedArea = [](const WindowPoint &p0, const WindowPoint &p1, const WindowPoint &p2, double &bound) {
            const double dx = p1.x - p0.x, dy = p1.y - p0.y;
            const double ex = p2.x - p0.x, ey = p2.y - p0.y;
            const double s = std::max({p0.slack, p1.slack, p2.slack});
            bound = 2.0 * s * (std::fabs(dx) + std::fabs(dy) + std::fabs(ex) + std::fabs(ey)) + 8.0 * s * s;
            return dx * ey - dy * ex;
        };
        double errA = 0, errB = 0;
        const double cA = signedArea(u, v, a, errA);
        const double cB = signedArea(u, v, b, errB);
        if (!std::isfinite(cA) || !std::isfinite(cB) || !std::isfinite(errA) || !std::isfinite(errB))
            return false;
        return (cA > errA && cB < -errB) || (cA < -errA && cB > errB);
    }

    static bool bounds(const DrawGeometry &g, PixelBox &out)
    {
        Analysis analysis(g);
        return analysis.bounds(out);
    }

    // Whether the draw covers every pixel of [0, w) x [0, h) for sure: all its vertices one axis-aligned rectangle (a
    // triangle `list` of 6, or a strip of 4 with no restart), within a scissor and viewport that hold the whole area
    static bool covers(const DrawGeometry &g, bool list, uint32_t w, uint32_t h)
    {
        Analysis analysis(g);
        return analysis.covers(list, w, h);
    }

private:
    static bool boxesShareTile(const PixelBox &a, const PixelBox &b)
    {
        return (a.x0 >> kTileShift) <= ((b.x1 - 1u) >> kTileShift) &&
               (b.x0 >> kTileShift) <= ((a.x1 - 1u) >> kTileShift) &&
               (a.y0 >> kTileShift) <= ((b.y1 - 1u) >> kTileShift) &&
               (b.y0 >> kTileShift) <= ((a.y1 - 1u) >> kTileShift);
    }

    static uint64_t rowMask(uint32_t tx0, uint32_t tx1)
    {
        return (~0ull >> (63u - (tx1 - tx0))) << tx0;
    }

    void recordBox(const PixelBox &box)
    {
        if (box.empty())
            return;
        if (box.x0 < kGridMax && box.y0 < kGridMax) {
            const uint32_t tx0 = box.x0 >> kTileShift;
            const uint32_t tx1 = std::min(box.x1 - 1, kGridMax - 1) >> kTileShift;
            const uint32_t ty0 = box.y0 >> kTileShift;
            const uint32_t ty1 = std::min(box.y1 - 1, kGridMax - 1) >> kTileShift;
            const uint64_t mask = rowMask(tx0, tx1);
            dirtyTileRows_ |= rowMask(ty0, ty1);
            for (uint32_t ty = ty0; ty <= ty1; ++ty)
                tiles_[ty] |= mask;
        }
        if (box.x1 > kGridMax || box.y1 > kGridMax) {
            if (!outOfGrid_) {
                outOfGridBox_ = box;
                outOfGrid_ = true;
            } else {
                outOfGridBox_.x0 = std::min(outOfGridBox_.x0, box.x0);
                outOfGridBox_.y0 = std::min(outOfGridBox_.y0, box.y0);
                outOfGridBox_.x1 = std::max(outOfGridBox_.x1, box.x1);
                outOfGridBox_.y1 = std::max(outOfGridBox_.y1, box.y1);
            }
        }
        if (!tileOnly_) {
            if (boxCount_ < kMaxDraws)
                boxes_[boxCount_++] = box;
            else
                tileOnly_ = true;
        }
    }

    bool tilesOverlap(const PixelBox &box) const
    {
        if (box.x0 >= kGridMax || box.y0 >= kGridMax)
            return false;
        const uint32_t tx0 = box.x0 >> kTileShift;
        const uint32_t tx1 = std::min(box.x1 - 1, kGridMax - 1) >> kTileShift;
        const uint32_t ty0 = box.y0 >> kTileShift;
        const uint32_t ty1 = std::min(box.y1 - 1, kGridMax - 1) >> kTileShift;
        const uint64_t mask = rowMask(tx0, tx1);
        for (uint32_t ty = ty0; ty <= ty1; ++ty)
            if (tiles_[ty] & mask)
                return true;
        return false;
    }

    void materializePending()
    {
        if (!overflowLimit_.empty()) {
            recordBox(overflowLimit_);
            overflowLimit_ = {};
        }
        if (pending_.empty())
            return;
        for (Analysis &a : pending_) {
            PixelBox box;
            recordBox(a.bounds(box) ? box : a.geometry().limit);
        }
        pending_.clear();
    }

    std::vector<Analysis> pending_;
    PixelBox overflowLimit_ = {};
    std::array<PixelBox, kMaxDraws> boxes_ = {};
    std::array<uint64_t, kTileGrid> tiles_ = {};
    uint64_t dirtyTileRows_ = 0;
    PixelBox outOfGridBox_ = {};
    uint32_t boxCount_ = 0;
    bool tileOnly_ = false;
    bool outOfGrid_ = false;
    bool lastNonOverlappingQuad_ = false;
};

}  // namespace gm20b
