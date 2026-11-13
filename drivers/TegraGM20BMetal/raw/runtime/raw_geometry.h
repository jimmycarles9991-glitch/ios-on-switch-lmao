// SPDX-License-Identifier: MIT
// Submission-local geometry snapshots. The caller owns a CPU read lease until GPU publication; an unavailable input
// is unbounded. Cached points never survive that ownership interval. Limits bound total CPU work, not just one draw.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "raw_bounds.h"

namespace raw {
struct DrawGeometry {
    bool bounded = false;
    const uint8_t *matrix = nullptr;
    PositionStream positions;
    uint64_t positionBytes = 0;
    uint32_t first = 0, count = 0;
    const uint8_t *indices = nullptr;
    uint32_t indexSize = 0;
    WindowViewport viewport;
    PixelBox limit;
    bool strip = true;
    // Acquisition is lazy: a draw that never needs a CPU geometry proof performs no ownership syscall.
    void *owner = nullptr;
    std::array<const void *, 3> inputs = {};
    bool (*acquire)(void *, const void *, const uint8_t *, uint64_t) = nullptr;
};

struct GeometryBudget {
    uint32_t vertices = 4096, pairs = 4096;
    uint64_t maintainedBytes = 256 * 1024;

    bool takeVertices(uint32_t count)
    {
        if (count > vertices)
            return false;
        vertices -= count;
        return true;
    }

    bool takePair()
    {
        if (!pairs)
            return false;
        --pairs;
        return true;
    }
};

struct GeometryVertex {
    WindowPoint at;
    uint32_t bits[4] = {};
    bool restart = false;
};

class GeometrySnapshot {
public:
    static constexpr uint32_t kMaxVertices = 288;

    explicit GeometrySnapshot(const DrawGeometry &g, GeometryBudget *budget = nullptr) : source(g), budget_(budget)
    {}

    // another draw's, its lists keeping their capacity
    void reset(const DrawGeometry &g, GeometryBudget *budget)
    {
        source = g;
        budget_ = budget;
        points.clear();
        runs.clear();
        evaluated = valid = exactX = exactY = runsEvaluated = runsValid = false;
    }

    bool evaluate()
    {
        if (evaluated)
            return valid;
        evaluated = true;
        const auto &g = source;
        if (!g.bounded || !g.matrix || !g.positions.base || g.count > kMaxVertices || g.positions.components < 2 ||
            g.positions.components > 4 || (g.indices && g.indexSize != 2 && g.indexSize != 4) ||
            (budget_ && !budget_->takeVertices(g.count)))
            return false;
        auto acquire = [&](size_t input, const uint8_t *cpu, uint64_t bytes) {
            return !g.acquire || !g.inputs[input] || !bytes || g.acquire(g.owner, g.inputs[input], cpu, bytes);
        };
        if (!acquire(0, g.matrix, 64) || !acquire(2, g.indices, uint64_t(g.count) * g.indexSize))
            return false;
        points.resize(g.count);
        std::array<uint32_t, kMaxVertices> vertices = {};
        const uint64_t reach = uint64_t(g.positions.offset) + g.positions.components * sizeof(float);
        uint64_t low = UINT64_MAX, high = 0;
        for (uint32_t i = 0; i < g.count; ++i) {
            auto &point = points[i];
            uint32_t index = i;
            if (g.indices) {
                if (g.indexSize == 2) {
                    uint16_t word;
                    memcpy(&word, g.indices + uint64_t(i) * 2, 2);
                    index = word;
                    point.restart = g.strip && word == UINT16_MAX;
                } else {
                    memcpy(&index, g.indices + uint64_t(i) * 4, 4);
                    point.restart = g.strip && index == UINT32_MAX;
                }
                if (point.restart)
                    continue;
            }
            // The GPU adds the signed base vertex in 32 bits as well.
            vertices[i] = g.first + index;
            const uint64_t at = uint64_t(vertices[i]) * g.positions.stride;
            if (at > g.positionBytes || reach > g.positionBytes - at)
                return false;
            low = std::min(low, at + g.positions.offset);
            high = std::max(high, at + reach);
        }
        if (high && !acquire(1, g.positions.base + low, high - low))
            return false;
        float matrix[16];
        memcpy(matrix, g.matrix, sizeof matrix);
        exactX = matrix[4] == 0 && matrix[7] == 0;
        exactY = matrix[1] == 0 && matrix[3] == 0;
        const DrawBounds bounds(matrix, g.viewport);
        for (uint32_t i = 0; i < g.count; ++i) {
            auto &point = points[i];
            if (point.restart)
                continue;
            const uint64_t at = uint64_t(vertices[i]) * g.positions.stride;
            // Read each position once from the acquired mapping; projection and exact-edge comparisons use cached bytes.
            memcpy(point.bits, g.positions.base + at + g.positions.offset, g.positions.components * sizeof(float));
            const PositionStream scratch{reinterpret_cast<const uint8_t *>(point.bits), 16, 0, g.positions.components};
            if (!bounds.project(scratch, 0, point.at))
                return false;
        }
        valid = true;
        return true;
    }

    bool takePair()
    {
        return !budget_ || budget_->takePair();
    }

    // Match the original draw or a contiguous run of it; the scissor does not change its projected vertices.
    bool contains(const DrawGeometry &g, uint32_t &begin) const
    {
        const auto &s = source;
        if (s.bounded != g.bounded || s.matrix != g.matrix || s.positions.base != g.positions.base ||
            s.positions.stride != g.positions.stride || s.positions.offset != g.positions.offset ||
            s.positions.components != g.positions.components || s.positionBytes != g.positionBytes ||
            s.indexSize != g.indexSize || s.strip != g.strip || s.viewport.originX != g.viewport.originX ||
            s.viewport.originY != g.viewport.originY || s.viewport.width != g.viewport.width ||
            s.viewport.height != g.viewport.height || bool(s.indices) != bool(g.indices) || s.owner != g.owner ||
            s.inputs != g.inputs || s.acquire != g.acquire)
            return false;
        if (s.indices) {
            if (g.first != s.first || !g.indexSize || uintptr_t(g.indices) < uintptr_t(s.indices))
                return false;
            const uint64_t bytes = uintptr_t(g.indices) - uintptr_t(s.indices);
            if (bytes % g.indexSize || bytes / g.indexSize > s.count)
                return false;
            begin = uint32_t(bytes / g.indexSize);
        } else {
            if (g.first < s.first)
                return false;
            begin = g.first - s.first;
        }
        return begin <= s.count && g.count <= s.count - begin;
    }

    DrawGeometry source;
    std::vector<GeometryVertex> points;
    std::vector<std::pair<uint32_t, uint32_t>> runs;
    bool evaluated = false, valid = false, exactX = false, exactY = false;
    bool runsEvaluated = false, runsValid = false;

private:
    GeometryBudget *budget_ = nullptr;
};

class GeometryAnalysis {
public:
    explicit GeometryAnalysis(const DrawGeometry &g)
        : GeometryAnalysis(g, g.bounded ? std::make_shared<GeometrySnapshot>(g) : nullptr, 0)
    {}

    GeometryAnalysis(const DrawGeometry &g, std::shared_ptr<GeometrySnapshot> snapshot, uint32_t begin)
        : geometry_(g), snapshot_(std::move(snapshot)), begin_(begin)
    {}

    const DrawGeometry &geometry() const
    {
        return geometry_;
    }

    bool evaluated() const
    {
        return evaluated_;
    }

    PixelBox cachedBox() const
    {
        return valid_ ? pixels_ : geometry_.limit;
    }

    GeometrySnapshot &snapshot()
    {
        return *snapshot_;
    }

    uint32_t begin() const
    {
        return begin_;
    }

    bool bounds(PixelBox &out)
    {
        if (!evaluated_) {
            evaluated_ = true;
            if (snapshot_ && snapshot_->evaluate()) {
                const float unused[16] = {};
                bounds_.emplace(unused, geometry_.viewport);
                for (uint32_t i = 0; i < geometry_.count; ++i) {
                    const auto &point = snapshot_->points[begin_ + i];
                    if (!point.restart) {
                        bounds_->addProjected(point.at);
                        ++added_;
                    }
                }
                pixels_ = bounds_->pixels(geometry_.limit);
                valid_ = true;
            }
        }
        if (valid_)
            out = pixels_;
        return valid_;
    }

    bool covers(bool list, uint32_t w, uint32_t h)
    {
        const auto &g = geometry_;
        if (!g.bounded || g.count != (list ? 6u : 4u) || g.limit.x0 || g.limit.y0 || g.limit.x1 < w || g.limit.y1 < h)
            return false;
        PixelBox box, certain, possible;
        return bounds(box) && added_ == g.count && bounds_->rectangle(list, certain, possible) && !certain.x0 &&
               !certain.y0 && certain.x1 >= w && certain.y1 >= h;
    }

private:
    DrawGeometry geometry_;
    std::shared_ptr<GeometrySnapshot> snapshot_;
    std::optional<DrawBounds> bounds_;
    PixelBox pixels_;
    uint32_t begin_ = 0, added_ = 0;
    bool evaluated_ = false, valid_ = false;
};

// A command buffer's snapshots, reused by the next one: every analysis that names one is its encoders', and they are
// all made again before the pool is cleared at the next command buffer's begin. The pointers it hands out own nothing
// (an empty owner: no allocation, no reference counts).
class GeometrySnapshotPool {
public:
    void clear()
    {
        used_ = 0;
    }

    std::shared_ptr<GeometrySnapshot> take(const DrawGeometry &g, GeometryBudget *budget)
    {
        if (used_ == snapshots_.size())
            snapshots_.emplace_back(g, budget);
        else
            snapshots_[used_].reset(g, budget);
        return std::shared_ptr<GeometrySnapshot>(std::shared_ptr<GeometrySnapshot>(), &snapshots_[used_++]);
    }

private:
    std::deque<GeometrySnapshot> snapshots_;
    size_t used_ = 0;
};

class GeometryCache {
public:
    GeometryAnalysis analysis(const DrawGeometry &g, uint64_t revision, GeometryBudget &budget,
                              GeometrySnapshotPool *pool = nullptr)
    {
        if (!g.bounded)
            return GeometryAnalysis(g, nullptr, 0);
        for (const auto &entry : entries_) {
            uint32_t begin = 0;
            if (entry.snapshot && entry.revision == revision && entry.snapshot->contains(g, begin))
                return GeometryAnalysis(g, entry.snapshot, begin);
        }
        auto snapshot = pool ? pool->take(g, &budget) : std::make_shared<GeometrySnapshot>(g, &budget);
        entries_[next_] = {revision, snapshot};
        next_ = (next_ + 1) % entries_.size();
        return GeometryAnalysis(g, std::move(snapshot), 0);
    }

private:
    struct Entry {
        uint64_t revision = 0;
        std::shared_ptr<GeometrySnapshot> snapshot;
    };

    std::array<Entry, 4> entries_;
    size_t next_ = 0;
};
}  // namespace raw
