// SPDX-License-Identifier: MIT
// DEVELOPMENT: what the scroll edge keyline's purple row (hw_test_app ui.pocket_keyline) is made of. A Core Animation draw
// as the render server made it: its quads as Core Animation wrote them (the position through the float4x4 at vertex buffer
// 2 into the viewport's pixels, attribute 3's half4 colour) and the thin ones among them, for the draw's line in
// raw_gpu.cpp probeDraw. And a write that stops a row or two short of a texture's edge (EdgeMiss): rows nothing writes
// keep what the memory held, Metal's fresh texture texel where m2v made the texture.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace raw {

// The vertices a draw's scan reads at most
constexpr uint32_t kFillScanVertices = 1024;

struct FillStream {
    const uint8_t *bytes = nullptr;  // from the draw's first vertex
    uint64_t size = 0;
    uint32_t stride = 0, positionOffset = 0, positionComponents = 0;
    int32_t colorOffset = -1;  // a half4; -1: none
};

struct FillQuad {
    uint32_t first = 0;  // its first vertex, from the draw's
    float box[4] = {};   // x0, y0, x1, y1 in pixels, y down
    float color[4] = {};
    bool colored = false;
};

// A draw's quads, `group` vertices each, and the first one at most two pixels across in x or in y (the keyline is one row
// of the pocket)
struct FillScan {
    uint32_t quads = 0, thin = 0;
    FillQuad first, firstThin;
};

// The half4 colour's offset of a state's key (" a3=27:<offset>:..."), else -1
inline int32_t fillColorOffset(const char *key)
{
    const char *at = key ? strstr(key, " a3=") : nullptr;
    unsigned format = 0, offset = 0;
    return at && sscanf(at, " a3=%u:%u", &format, &offset) == 2 && format == 27 ? int32_t(offset) : -1;
}

inline float fillHalf(uint16_t h)
{
    const uint32_t exponent = h >> 10 & 0x1f, mantissa = h & 0x3ff;
    float magnitude;
    if (exponent == 0x1f)
        magnitude = mantissa ? NAN : INFINITY;
    else if (exponent == 0)
        magnitude = std::ldexp(float(mantissa), -24);
    else
        magnitude = std::ldexp(float(mantissa | 0x400), int(exponent) - 25);
    return h >> 15 ? -magnitude : magnitude;
}

// Vertex `i` in the viewport's pixels (`matrix`: column-major; none: the position is clip space), false past the stream
inline bool fillWindow(const FillStream &s, uint32_t i, const float *matrix, const double viewport[4], float out[2])
{
    const uint64_t at = uint64_t(i) * s.stride + s.positionOffset;
    if (at + sizeof(float) * s.positionComponents > s.size)
        return false;
    float p[4] = {0, 0, 0, 1};
    memcpy(p, s.bytes + at, sizeof(float) * s.positionComponents);
    float clip[4] = {p[0], p[1], p[2], p[3]};
    if (matrix)
        for (int r = 0; r < 4; r++)
            clip[r] = matrix[r] * p[0] + matrix[4 + r] * p[1] + matrix[8 + r] * p[2] + matrix[12 + r] * p[3];
    const float w = clip[3] != 0 ? clip[3] : 1;
    out[0] = float(viewport[0] + (clip[0] / w + 1) * 0.5 * viewport[2]);
    out[1] = float(viewport[1] + (1 - clip[1] / w) * 0.5 * viewport[3]);
    return true;
}

inline void fillColor(const FillStream &s, uint32_t i, FillQuad &q)
{
    const uint64_t at = uint64_t(i) * s.stride + uint64_t(s.colorOffset);
    if (s.colorOffset < 0 || at + 8 > s.size)
        return;
    uint16_t h[4];
    memcpy(h, s.bytes + at, sizeof h);
    for (int c = 0; c < 4; c++)
        q.color[c] = fillHalf(h[c]);
    q.colored = true;
}

// `count` vertices from the stream's first (or `indices` of them), at most kFillScanVertices, `group` a quad (a strip's
// whole draw, a list's two triangles); `matrix`: vertex buffer 2's 64 bytes
inline FillScan fillScan(const FillStream &s, const uint32_t *indices, uint32_t count, uint32_t group,
                         const uint8_t *matrix, const double viewport[4])
{
    FillScan out;
    if (!s.bytes || !s.stride || !group || s.positionComponents < 2 || s.positionComponents > 4)
        return out;
    float m[16] = {};
    if (matrix)
        memcpy(m, matrix, sizeof m);
    const uint32_t vertices = std::min(count, kFillScanVertices);
    for (uint32_t first = 0; first + group <= vertices; first += group) {
        FillQuad q;
        q.first = first;
        for (uint32_t i = first; i < first + group; i++) {
            float at[2];
            if (!fillWindow(s, indices ? indices[i] : i, matrix ? m : nullptr, viewport, at))
                return out;
            if (i == first) {
                q.box[0] = q.box[2] = at[0];
                q.box[1] = q.box[3] = at[1];
            }
            q.box[0] = std::fmin(q.box[0], at[0]);
            q.box[1] = std::fmin(q.box[1], at[1]);
            q.box[2] = std::fmax(q.box[2], at[0]);
            q.box[3] = std::fmax(q.box[3], at[1]);
        }
        fillColor(s, indices ? indices[first] : first, q);
        if (!out.quads++)
            out.first = q;
        if ((q.box[2] - q.box[0] <= 2 || q.box[3] - q.box[1] <= 2) && !out.thin++)
            out.firstThin = q;
    }
    return out;
}

// How far a write of [x0, x1) x [y0, y1) falls short of each edge of a w x h texture, where every shortfall is at most two
// rows or columns and one is not zero; all zero otherwise (the rectangle reaches every edge, or misses one by more)
struct EdgeMiss {
    uint32_t top = 0, bottom = 0, left = 0, right = 0;

    constexpr bool any() const
    {
        return top || bottom || left || right;
    }
};

constexpr EdgeMiss edgeMiss(uint32_t w, uint32_t h, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    if (x1 <= x0 || y1 <= y0 || x1 > w || y1 > h)
        return {};
    const EdgeMiss m{y0, h - y1, x0, w - x1};
    return m.top <= 2 && m.bottom <= 2 && m.left <= 2 && m.right <= 2 ? m : EdgeMiss{};
}

}  // namespace raw
