// SPDX-License-Identifier: MIT
// setViewports:count: and setScissorRects:count: as the words NVK writes for each index (nvk_cmd_draw.c nvk_emit_viewport,
// nvk_emit_scissor: SET_VIEWPORT_SCALE/OFFSET(j), SET_VIEWPORT_CLIP_*(j), SET_SCISSOR_*(j)), m2v's Vulkan viewport of each
// Metal one (y flipped: y + h, height -h) and each rectangle clipped to the render area as plain Metal clips it
// (render_state.rs set_scissors). A vertex function's [[viewport_array_index]] selects one below the encoder's count
// (Metal draws an index at or past it through viewport 0); without one every primitive takes index 0.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace raw {

// The most an encoder sets: GM20B's viewports and scissors (SET_VIEWPORT_*(j), j < 16), as many as the A13 answers
// maxViewportCount; the A10X answers 1 and its driver keeps every viewport it is given
constexpr uint32_t kMaxViewports = 16;

// The vertex program writes ViewportIndex (or Layer): its shader header's output map, NAK's sysvals_out bit of attribute
// 0x068 (0x064) in OMAP_SYSTEM_VALUES_A at header bit 400 (nak/sph.rs)
inline bool headerWritesViewportIndex(const uint8_t *sph)
{
    return sph && (sph[(400 + 0x068 / 4) / 8] >> ((400 + 0x068 / 4) % 8) & 1);
}

inline bool headerWritesLayer(const uint8_t *sph)
{
    return sph && (sph[(400 + 0x064 / 4) / 8] >> ((400 + 0x064 / 4) % 8) & 1);
}

inline uint32_t viewportBits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// Metal's viewport (x, y, w, h, znear, zfar) as the eight words raw keeps: scale X Y Z, offset X Y Z, then each clip's
// x | width << 16 (RenderEncoder::setViewport's)
inline void viewportWords(const double v[6], uint32_t w[8])
{
    const double x = v[0], y = v[1], width = v[2], h = v[3], znear = v[4], zfar = v[5];
    const float vy = float(y + h), vh = float(-h);
    w[0] = viewportBits(0.5f * float(width));
    w[1] = viewportBits(0.5f * vh);
    w[2] = viewportBits(float(zfar - znear));
    w[3] = viewportBits(float(x) + 0.5f * float(width));
    w[4] = viewportBits(vy + 0.5f * vh);
    w[5] = viewportBits(float(znear));
    const float xmin = std::clamp(float(x), 0.0f, 65535.0f), xmax = std::clamp(float(x + width), 0.0f, 65535.0f);
    const float ymin = std::clamp(std::min(vy, vy + vh), 0.0f, 65535.0f),
                ymax = std::clamp(std::max(vy, vy + vh), 0.0f, 65535.0f);
    w[6] = uint32_t(xmax - xmin) << 16 | uint32_t(xmin);
    w[7] = uint32_t(ymax - ymin) << 16 | uint32_t(ymin);
}

// A scissor rectangle (x, y, w, h) inside the render area: [x0, x1) x [y0, y1)
struct ScissorBox {
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

inline ScissorBox scissorBox(const uint32_t r[4], uint32_t areaW, uint32_t areaH)
{
    ScissorBox s;
    s.x0 = std::min(r[0], areaW);
    s.y0 = std::min(r[1], areaH);
    s.x1 = uint32_t(std::min<uint64_t>(uint64_t(r[0]) + r[2], areaW));
    s.y1 = uint32_t(std::min<uint64_t>(uint64_t(r[1]) + r[3], areaH));
    return s;
}

// The pixels a draw through one viewport and scissor may write: its scissor within its viewport's pixels (m2v vk_scissors,
// viewport_pixels), [x[0], x[1]) x [y[0], y[1])
inline void viewportPixels(const double vp[4], const ScissorBox &sc, uint32_t areaW, uint32_t areaH, uint32_t x[2],
                           uint32_t y[2])
{
    auto axis = [](double origin, double size, uint32_t limit, uint32_t lo, uint32_t hi, uint32_t out[2]) {
        const double a = size < 0 ? origin + size : origin, b = size < 0 ? origin : origin + size;
        const int64_t first = int64_t(std::clamp(std::floor(a), 0.0, double(limit)));
        const int64_t end =
            size == 0 ? first : std::max<int64_t>(first, int64_t(std::clamp(std::ceil(b), 0.0, double(limit))));
        out[0] = uint32_t(std::max<int64_t>(lo, first));
        out[1] = std::max(out[0], uint32_t(std::min<int64_t>(hi, end)));
    };
    const bool empty = vp[2] == 0 || vp[3] == 0;
    axis(vp[0], empty ? 0 : vp[2], areaW, sc.x0, sc.x1, x);
    axis(vp[1], empty ? 0 : vp[3], areaH, sc.y0, sc.y1, y);
}

// Indices 1 .. count - 1 of the encoder's viewports and scissors (index 0 is the encoder's own): their words, what they
// cover, and which of them the next draw writes
struct ViewportArray {
    uint32_t viewports = 1, scissors = 1;
    uint32_t viewport[kMaxViewports][8] = {};
    double vp[kMaxViewports][4] = {};
    ScissorBox scissor[kMaxViewports];
    uint32_t dirtyViewports = 0, dirtyScissors = 0;
    uint32_t swizzled = 1;  // a turned pass: the indices whose clip-space swizzle is the turn (index 0 at its start)
};

}  // namespace raw
