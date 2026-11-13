// SPDX-License-Identifier: MIT
// The fetch barrier's region test: whether a fetching draw's pixels can meet those a draw since the last barrier wrote,
// newest first, until one meets it. A draw is its pixel limit (scissor within viewport) unless its vertices were bounded
// already (a pass's first draw, for its cover test): the test reads no vertex.
//
// Skipping the barrier where no written pixel is fetched is exact on uncompressed (pitch and generic block-linear)
// targets: a texel's bytes change only where the ROP writes that pixel, so a fetch of any other pixel reads the same
// bytes whether or not the earlier writes have landed, from the texture cache or from L2.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include "raw_geometry.h"

namespace raw {

class WrittenRegion {
public:
    static constexpr uint32_t kMaxDraws = 16, kMaxVertices = GeometrySnapshot::kMaxVertices;
    using Analysis = GeometryAnalysis;

    void clear()
    {
        draws_.clear();
        overflow_ = false;
    }

    // the list, its capacity for another region (the render encoder's next pass: no allocation a steady frame)
    std::vector<Analysis> release()
    {
        clear();
        return std::move(draws_);
    }

    void adopt(std::vector<Analysis> &&storage)
    {
        draws_ = std::move(storage);
        clear();
    }

    void add(const DrawGeometry &g)
    {
        add(Analysis(g));
    }

    void add(const Analysis &analysis)
    {
        if (analysis.geometry().limit.empty())
            return;
        if (draws_.size() == kMaxDraws) {
            overflow_ = true;
            return;
        }
        draws_.push_back(analysis);
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
        if (overflow_)
            return true;
        const PixelBox box = analysis.cachedBox();
        for (size_t i = draws_.size(); i-- > 0;)
            if (draws_[i].cachedBox().overlaps(box))
                return true;
        return false;
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
    std::vector<Analysis> draws_;
    bool overflow_ = false;
};

}  // namespace raw
