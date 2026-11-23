// SPDX-License-Identifier: MIT
// The loaded passes harness/ycbcrloadoracle.mm runs on this Mac's Metal and ycbcr_load_test holds the model to: the
// targets, their planes' bytes (a row's codes, no pitch), the drawn floats and rectangles, and the model of the driver's
// route (ycbcr_load.h): the load state's stand-in, the draws, the store rule and the tiles it writes.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../ycbcr_load.h"

namespace ycbcrload {

struct Case {
    uint32_t format;
    uint32_t fourcc;
    const char *matrix;
};

constexpr uint32_t fourcc(const char (&c)[5])
{
    return uint32_t(uint8_t(c[0])) << 24 | uint32_t(uint8_t(c[1])) << 16 | uint32_t(uint8_t(c[2])) << 8 | uint8_t(c[3]);
}

inline std::string fourccText(uint32_t f)
{
    const char s[5] = {char(f >> 24), char(f >> 16), char(f >> 8), char(f), 0};
    return s;
}

inline std::vector<Case> cases()
{
    const std::pair<uint32_t, const char *> all[] = {
        {540, "420v"}, {540, "420f"}, {541, "422v"}, {541, "422f"}, {542, "444v"}, {543, "x420"}, {543, "xf20"},
        {544, "x422"}, {544, "xf22"}, {545, "x444"}, {545, "xf44"}, {500, "420v"}, {500, "420f"}, {502, "422v"},
        {502, "422f"}, {503, "444v"}, {503, "444f"}, {505, "x420"}, {505, "xf20"}, {506, "x422"}, {506, "xf22"},
        {507, "x444"}, {507, "xf44"}, {520, "420v"}, {520, "420f"}, {522, "422v"}, {522, "422f"}, {523, "444v"},
        {523, "444f"}, {525, "x420"}, {525, "xf20"}, {526, "x422"}, {526, "xf22"}, {527, "x444"}, {527, "xf44"},
        {508, "p420"}, {509, "p422"}, {510, "p444"}, {528, "pf20"}, {529, "p422"}, {530, "pf44"}, {546, "p420"},
        {547, "pf22"}, {548, "p444"}};
    std::vector<Case> out;
    for (const auto &[format, text] : all) {
        const uint32_t f = uint32_t(uint8_t(text[0])) << 24 | uint32_t(uint8_t(text[1])) << 16 |
                           uint32_t(uint8_t(text[2])) << 8 | uint8_t(text[3]);
        const bool converted = format < 540 && !(format >= 508 && format <= 510) && !(format >= 528 && format <= 530);
        out.push_back({format, f, nullptr});
        if (converted)
            for (const char *m : {"ITU_R_709_2", "ITU_R_2020"})
                out.push_back({format, f, m});
    }
    return out;
}

// The cases ycbcr_load_test holds the model to, with the hash of Metal's planes after kGoldenArm on an M4 Max
inline std::vector<Case> goldenCases()
{
    return {{540, fourcc("420v"), nullptr},      {500, fourcc("420f"), "ITU_R_709_2"}, {520, fourcc("420v"), nullptr},
            {545, fourcc("x444"), nullptr},      {503, fourcc("444f"), nullptr},       {541, fourcc("422v"), nullptr},
            {543, fourcc("xf20"), nullptr},      {525, fourcc("x420"), "ITU_R_2020"},  {527, fourcc("xf44"), nullptr},
            {506, fourcc("x422"), "ITU_R_709_2"}};
}

using Size = std::array<uint32_t, 2>;

inline std::vector<Size> sizes(bool golden)
{
    return golden ? std::vector<Size>{{64, 48}} : std::vector<Size>{{128, 64}, {66, 34}};
}

struct Arm {
    uint32_t x = 0, y = 0, w = 0, h = 0;
    bool draws = true;
    bool second = false;  // beside a second attachment cleared and stored
    const char *label = "";
};

constexpr Arm kGoldenArm = {13, 9, 30, 20, true, false, "golden"};

inline std::vector<Arm> arms(uint32_t w, uint32_t h)
{
    return {{13, 9, w / 2 + 1, h / 2 - 1, true, false, "rect 13,9"},
            {40, 20, 8, 8, true, false, "rect 40,20"},
            {33, 1, 2, 2, true, false, "rect 33,1"},
            {0, 0, w, h, true, false, "whole"},
            {w - 5, h - 3, 5, 3, true, false, "corner"},
            {31, 31, 2, 2, true, false, "four tiles"},
            {0, 0, 0, 0, false, false, "no draw"},
            {40, 20, 8, 8, true, true, "beside a cleared attachment"}};
}

inline std::string name(const Case &c, const Size &size)
{
    return std::to_string(c.format) + " over " + fourccText(c.fourcc) + " " + (c.matrix ? c.matrix : "no matrix") +
           " " + std::to_string(size[0]) + "x" + std::to_string(size[1]);
}

// Each plane's rows of codes, no pitch: a byte, a little-endian 16-bit word (a 10-bit code its high bits) or a packed word
struct Layout {
    uint32_t width = 0, height = 0;
    uint32_t rowBytes[2] = {}, rows[2] = {}, pitch[2] = {};
    uint32_t codeBytes = 1;  // 1, 2, or 4: three 10-bit codes to a word
    uint32_t codes[2] = {};  // codes a row
};

using Planes = std::array<std::vector<uint8_t>, 2>;

inline Layout layoutOf(const gm20b::YcbcrPlan &p, uint32_t w, uint32_t h)
{
    Layout l;
    l.width = w;
    l.height = h;
    l.codeBytes = p.lumaFormat == 53 ? 4 : p.lumaFormat == 20 ? 2 : 1;
    l.codes[0] = w;
    l.codes[1] = 2 * (p.store.across ? w / 2 : w);
    l.rows[0] = h;
    l.rows[1] = p.chromaHeight;
    for (int k = 0; k < 2; k++)
        l.rowBytes[k] = l.codeBytes == 4 ? 4 * ((l.codes[k] + 2) / 3) : l.codeBytes * l.codes[k];
    l.pitch[0] = p.lumaPitch;
    l.pitch[1] = p.chromaPitch;
    return l;
}

template <typename Base> Planes readPlanes(const Layout &l, Base base)
{
    Planes out;
    for (uint32_t k = 0; k < 2; k++)
        for (uint32_t y = 0; y < l.rows[k]; y++) {
            const uint8_t *row = base(k) + size_t(y) * l.pitch[k];
            out[k].insert(out[k].end(), row, row + l.rowBytes[k]);
        }
    return out;
}

template <typename Base> void writePlanes(const Layout &l, const Planes &p, Base base)
{
    for (uint32_t k = 0; k < 2; k++)
        for (uint32_t y = 0; y < l.rows[k]; y++)
            memcpy(base(k) + size_t(y) * l.pitch[k], &p[k][size_t(y) * l.rowBytes[k]], l.rowBytes[k]);
}

inline uint64_t splitmix(uint64_t &s)
{
    uint64_t z = (s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// Random codes; a packed word's slots past its row's codes and its top two bits zero, as Metal's reads ignore them
inline Planes randomPlanes(const Layout &l, uint64_t seed)
{
    Planes out;
    for (uint32_t k = 0; k < 2; k++) {
        out[k].resize(size_t(l.rowBytes[k]) * l.rows[k]);
        for (uint8_t &b : out[k])
            b = uint8_t(splitmix(seed));
        if (l.codeBytes != 4)
            continue;
        for (uint32_t y = 0; y < l.rows[k]; y++)
            for (uint32_t x = 0; x < l.rowBytes[k] / 4; x++) {
                uint32_t word;
                uint8_t *at = &out[k][size_t(y) * l.rowBytes[k] + 4 * x];
                memcpy(&word, at, 4);
                word &= 0x3fffffffu;
                for (uint32_t i = 0; i < 3; i++)
                    if (3 * x + i >= l.codes[k])
                        word &= ~(1023u << (10 * i));
                memcpy(at, &word, 4);
            }
    }
    return out;
}

// Floats a draw writes: beside half midpoints and the 1/255, 1/1023, 1/2048 and 1/8192 grids, and across [-0.05, 1.05]
inline std::vector<float> drawnValues(uint32_t texels, uint64_t seed)
{
    std::vector<float> v(4 * size_t(texels), 1.0f);
    const uint32_t grids[] = {255, 1023, 2048, 8192};
    for (uint32_t i = 0; i < texels; i++)
        for (int c = 0; c < 3; c++) {
            const uint64_t k = splitmix(seed);
            float f;
            switch (k % 3) {
            case 0: {
                const uint32_t g = grids[(k >> 2) % 4];
                f = float((double((k >> 8) % g) + 0.5) / g);
                break;
            }
            case 1: {
                const uint16_t h = uint16_t((k >> 8) % 0x3c00);
                _Float16 a, b;
                const uint16_t h1 = uint16_t(h + 1);
                memcpy(&a, &h, 2);
                memcpy(&b, &h1, 2);
                f = float((double(a) + double(b)) / 2);
                break;
            }
            default:
                f = float(double((k >> 11) % 1100001) / 1000000.0 - 0.05);
            }
            uint32_t u;
            memcpy(&u, &f, 4);
            u += uint32_t(int32_t((k >> 40) % 5) - 2);
            memcpy(&f, &u, 4);
            v[4 * size_t(i) + c] = f;
        }
    return v;
}

inline uint32_t code(const Layout &l, const std::vector<uint8_t> &plane, uint32_t k, uint32_t x, uint32_t y)
{
    const uint8_t *row = &plane[size_t(y) * l.rowBytes[k]];
    if (l.codeBytes == 1)
        return row[x];
    if (l.codeBytes == 2)
        return uint32_t(row[2 * x] | row[2 * x + 1] << 8) >> 6;
    uint32_t word;
    memcpy(&word, row + 4 * (x / 3), 4);
    return (word >> (10 * (x % 3))) & 1023;
}

// The tiles a rectangle's draw reaches (reach.h: those its scissor touches, every one beside a cleared attachment)
inline std::vector<uint8_t> tileMask(const Layout &l, const Arm &a)
{
    const uint32_t tx = gm20b::reachTiles(l.width), ty = gm20b::reachTiles(l.height);
    std::vector<uint8_t> mask(size_t(tx) * ty, 0);
    for (uint32_t y = 0; y < ty; y++)
        for (uint32_t x = 0; x < tx; x++) {
            const uint32_t x0 = x * gm20b::kReachTile, y0 = y * gm20b::kReachTile;
            mask[y * tx + x] = a.second || (a.x < x0 + gm20b::kReachTile && x0 < a.x + a.w &&
                                            a.y < y0 + gm20b::kReachTile && y0 < a.y + a.h);
        }
    return mask;
}

// The planes after the driver's route of a loaded pass with `a`'s draw of `values`: the load state's stand-in, the drawn
// texels in the rectangle, then the store states over the tiles the draw reaches, every other byte as `before` holds it
inline Planes modelPass(const Case &, const gm20b::YcbcrPlan &p, const Layout &l, const Planes &before,
                        const std::vector<float> &values, const Arm &a, const uint16_t *knots)
{
    if (!a.draws)
        return before;
    const uint32_t w = l.width, h = l.height;
    std::vector<uint16_t> luma(size_t(w) * h), chroma(size_t(l.codes[1]) * l.rows[1]);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            luma[y * w + x] = uint16_t(code(l, before[0], 0, x, y));
    for (uint32_t y = 0; y < l.rows[1]; y++)
        for (uint32_t x = 0; x < l.codes[1]; x++)
            chroma[y * l.codes[1] + x] = uint16_t(code(l, before[1], 1, x, y));
    const gm20b::YcbcrPlaneCodes codes = {luma.data(), chroma.data(), l.codes[1] / 2};
    std::vector<float> standIn(4 * size_t(w) * h);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            float *t = &standIn[4 * (size_t(y) * w + x)];
            const bool drawn = x >= a.x && x < a.x + a.w && y >= a.y && y < a.y + a.h;
            if (drawn)
                memcpy(t, &values[4 * (size_t(y) * w + x)], 16);
            else
                gm20b::ycbcrLoadTexel(p.output, p.shape, codes, knots, x, y, t);
        }
    uint16_t encode[gm20b::kYcbcrSrgbSteps + 1];
    gm20b::ycbcrSrgbEncodeTable(knots, encode);
    const std::vector<uint8_t> mask = tileMask(l, a);
    const uint32_t tx = gm20b::reachTiles(w), ty = gm20b::reachTiles(h);
    const uint32_t bw = p.store.across ? 2 : 1, bh = p.store.down ? 2 : 1;
    const auto pixelOf = [&](uint32_t k, uint32_t codeAt) {
        return k ? codeAt / 2 * bw : codeAt;
    };
    Planes out = before;
    if (l.codeBytes == 4) {
        std::vector<uint32_t> words[2] = {std::vector<uint32_t>(l.rowBytes[0] / 4 * h),
                                          std::vector<uint32_t>(l.rowBytes[1] / 4 * l.rows[1])};
        gm20b::ycbcrStoreDrawnPacked(p.store, encode, standIn.data(), w, h, words[0].data(), words[1].data());
        for (uint32_t k = 0; k < 2; k++)
            for (uint32_t y = 0; y < l.rows[k]; y++)
                for (uint32_t x = 0; x < l.rowBytes[k] / 4; x++) {
                    uint32_t mine = 0;
                    for (uint32_t i = 0; i < 3; i++)
                        if (3 * x + i < l.codes[k] &&
                            gm20b::ycbcrTileReached(mask.data(), tx, ty, pixelOf(k, 3 * x + i), k ? y * bh : y))
                            mine |= 1023u << (10 * i);
                    if (!mine)
                        continue;
                    uint8_t *at = &out[k][size_t(y) * l.rowBytes[k] + 4 * x];
                    uint32_t word;
                    memcpy(&word, at, 4);
                    word = (words[k][y * (l.rowBytes[k] / 4) + x] & mine) | (word & ~mine);
                    memcpy(at, &word, 4);
                }
        return out;
    }
    std::vector<uint16_t> stored[2] = {std::vector<uint16_t>(luma.size()), std::vector<uint16_t>(chroma.size())};
    gm20b::ycbcrStoreDrawn(p.store, encode, standIn.data(), w, h, stored[0].data(), stored[1].data());
    for (uint32_t k = 0; k < 2; k++)
        for (uint32_t y = 0; y < l.rows[k]; y++)
            for (uint32_t x = 0; x < l.codes[k]; x++) {
                if (!gm20b::ycbcrTileReached(mask.data(), tx, ty, pixelOf(k, x), k ? y * bh : y))
                    continue;
                const uint32_t c = stored[k][y * l.codes[k] + x];
                uint8_t *at = &out[k][size_t(y) * l.rowBytes[k] + l.codeBytes * x];
                if (l.codeBytes == 1) {
                    at[0] = uint8_t(c);
                } else {
                    at[0] = uint8_t(c << 6);
                    at[1] = uint8_t(c >> 2);
                }
            }
    return out;
}

// Bytes of `a` and `b` that differ, past those `skip` marks
inline uint64_t bytesOff(const Planes &a, const Planes &b, const Planes *skip = nullptr)
{
    uint64_t off = 0;
    for (int k = 0; k < 2; k++) {
        for (size_t i = 0; i < a[k].size() && i < b[k].size(); i++)
            off += a[k][i] != b[k][i] && !(skip && i < (*skip)[k].size() && (*skip)[k][i]);
        off += a[k].size() > b[k].size() ? a[k].size() - b[k].size() : b[k].size() - a[k].size();
    }
    return off;
}

// The chroma bytes Metal leaves undefined in a loaded pass: a block in a tile the pass stores whose Load mixes a block of
// another tile it stores. Metal's tiles load and store in an order of their own, so the block's texels read that neighbour
// before or after its tile's store (an M4 Max moves one such block in about three runs; the raw route reads every block
// before the pass, as a tile does that runs first).
inline Planes tileOrderUndefined(const Layout &l, const gm20b::YcbcrStoreShape &s, const Arm &a)
{
    Planes out = {std::vector<uint8_t>(size_t(l.rowBytes[0]) * l.rows[0]),
                  std::vector<uint8_t>(size_t(l.rowBytes[1]) * l.rows[1])};
    if (l.codeBytes == 4 || !(s.across || s.down))
        return out;
    const std::vector<uint8_t> stored = tileMask(l, a);
    const uint32_t tilesX = gm20b::reachTiles(l.width), bw = s.across ? 2 : 1, bh = s.down ? 2 : 1;
    const int blocks = int(l.codes[1] / 2), rows = int(l.rows[1]);
    const auto tileOf = [&](int bx, int by) {
        return size_t(uint32_t(by) * bh / gm20b::kReachTile) * tilesX + uint32_t(bx) * bw / gm20b::kReachTile;
    };
    for (int by = 0; by < rows; by++)
        for (int bx = 0; bx < blocks; bx++) {
            const size_t own = tileOf(bx, by);
            bool mixed = false;
            for (int dy = s.down ? -1 : 0; dy <= (s.down ? 1 : 0); dy++)
                for (int dx = s.across ? -1 : 0; dx <= (s.across ? 1 : 0); dx++) {
                    const size_t other = tileOf(std::clamp(bx + dx, 0, blocks - 1), std::clamp(by + dy, 0, rows - 1));
                    mixed |= other != own && stored[other];
                }
            if (mixed && stored[own])
                for (uint32_t b = 0; b < 2 * l.codeBytes; b++)
                    out[1][size_t(by) * l.rowBytes[1] + 2 * bx * l.codeBytes + b] = 1;
        }
    return out;
}

inline uint64_t hash(const Planes &p)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (const auto &plane : p)
        for (uint8_t b : plane)
            h = (h ^ b) * 0x100000001b3ull;
    return h;
}

}  // namespace ycbcrload
