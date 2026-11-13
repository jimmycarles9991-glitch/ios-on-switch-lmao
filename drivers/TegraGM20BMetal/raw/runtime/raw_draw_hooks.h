// SPDX-License-Identifier: MIT
// The CPU's reading of a draw that the ROP shortcuts need (raw_constant_colour.h, raw_quad_fill.h): one quad's corners
// from its six indices or four strip vertices, and whether every vertex the draw reads carries one colour. Read only from
// bytes the CPU owns (setVertexBytes uploads, indices under the command buffer's read lease), at most kFillScanVertices.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "raw_keyline_probe.h"

namespace raw {

// The quad two indexed triangles (a b c)(d e f) draw, as a strip's corners 0 1 2 3: the first triangle's vertex off the
// shared edge, the shared edge, the second's own vertex. False where they share fewer or more than two vertices.
inline bool quadOfIndices(const uint32_t i[6], uint32_t out[4])
{
    uint32_t shared[2], n = 0, lone = 0, own = 0;
    bool loneFound = false;
    for (int a = 0; a < 3; a++) {
        bool inSecond = false;
        for (int b = 3; b < 6; b++)
            inSecond |= i[a] == i[b];
        if (inSecond && n < 2)
            shared[n++] = i[a];
        else if (!inSecond) {
            lone = i[a];
            loneFound = true;
        }
    }
    if (n != 2 || !loneFound || shared[0] == shared[1])
        return false;
    int owns = 0;
    for (int b = 3; b < 6; b++)
        if (i[b] != shared[0] && i[b] != shared[1]) {
            own = i[b];
            owns++;
        }
    if (owns != 1 || own == lone)
        return false;
    out[0] = lone;
    out[1] = shared[0];
    out[2] = shared[1];
    out[3] = own;
    return true;
}

// The half4 colour every one of `count` vertices (or `indices` of them) carries, as float bits; false where two differ,
// the stream has no colour, or the draw reads more than kFillScanVertices
inline bool uniformColour(const FillStream &s, const uint32_t *indices, uint32_t count, uint32_t out[4])
{
    if (s.colorOffset < 0 || !s.stride || !count || count > kFillScanVertices)
        return false;
    uint16_t first[4] = {};
    for (uint32_t k = 0; k < count; k++) {
        const uint64_t at = uint64_t(indices ? indices[k] : k) * s.stride + uint64_t(s.colorOffset);
        if (at + sizeof first > s.size)
            return false;
        uint16_t h[4];
        memcpy(h, s.bytes + at, sizeof h);
        if (k == 0)
            memcpy(first, h, sizeof first);
        else if (memcmp(first, h, sizeof h) != 0)
            return false;
    }
    for (int c = 0; c < 4; c++) {
        const float f = fillHalf(first[c]);
        memcpy(&out[c], &f, sizeof f);
    }
    return true;
}

// The quad's corners in the viewport's pixels, strip order, for axisAlignedQuad; false past the stream
inline bool quadWindow(const FillStream &s, const uint32_t corners[4], const uint8_t *matrix, const double viewport[4],
                       float out[8])
{
    float m[16] = {};
    if (matrix)
        memcpy(m, matrix, sizeof m);
    for (int k = 0; k < 4; k++)
        if (!fillWindow(s, corners[k], matrix ? m : nullptr, viewport, out + 2 * k))
            return false;
    return true;
}

// Vertex attribute `index`'s byte offset in a state's key (" a<index>=<format>:<offset>"), where its format is `format`;
// else -1
inline int32_t vertexAttributeOffset(const char *key, uint32_t index, uint32_t format)
{
    char tag[8];
    snprintf(tag, sizeof tag, " a%u=", index);
    const char *at = key ? strstr(key, tag) : nullptr;
    unsigned f = 0, offset = 0;
    return at && sscanf(at + strlen(tag), "%u:%u", &f, &offset) == 2 && f == format ? int32_t(offset) : -1;
}

}  // namespace raw
