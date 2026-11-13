// SPDX-License-Identifier: MIT
#pragma once
#include "raw_semantic_state.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace raw {

inline uint64_t rasterPixelLimit(const RasterState &state, uint32_t width, uint32_t height)
{
    double x0 = 0, y0 = 0, x1 = width, y1 = height;
    if (state.set & RasterState::Scissor) {
        x0 = std::max(x0, double(state.scissor[0]));
        y0 = std::max(y0, double(state.scissor[1]));
        x1 = std::min(x1, double(uint64_t(state.scissor[0]) + state.scissor[2]));
        y1 = std::min(y1, double(uint64_t(state.scissor[1]) + state.scissor[3]));
    }
    if (state.set & RasterState::Viewport) {
        const auto number = [&](unsigned i) {
            double value;
            std::memcpy(&value, &state.viewport[i], sizeof(value));
            return value;
        };
        const double x = number(0), y = number(1), w = number(2), h = number(3);
        if (std::isfinite(x) && std::isfinite(y) && std::isfinite(w) && std::isfinite(h)) {
            x0 = std::max(x0, std::floor(x));
            y0 = std::max(y0, std::floor(y));
            x1 = std::min(x1, std::ceil(x + w));
            y1 = std::min(y1, std::ceil(y + h));
        }
    }
    return uint64_t(std::max(0.0, x1 - x0)) * uint64_t(std::max(0.0, y1 - y0));
}

}  // namespace raw
