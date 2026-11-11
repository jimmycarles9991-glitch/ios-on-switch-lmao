// SPDX-License-Identifier: MIT
// See fx_image.h. Layout rules: Mesa NIL (src/nouveau/nil/image.rs, tiling.rs, copy.rs; MIT).
#include "fx_image.h"

#include <algorithm>
#include <cstring>

namespace raw {
namespace fx {
namespace {

// Tiling::choose for ImageDim::_3D: the proprietary driver's block depth by the image's depth, which NIL copies
uint8_t chooseZLog2(uint32_t depth)
{
    return depth >= 11 ? 4 : depth >= 6 ? 3 : depth >= 3 ? 2 : depth >= 2 ? 1 : 0;
}

template <class Move>
void eachRun(const MipChain &c, uint32_t l, const uint32_t origin[3], const uint32_t size[3], Move move)
{
    const uint32_t first = origin[0] * c.bpp, end = (origin[0] + size[0]) * c.bpp;
    for (uint32_t plane = 0; plane < size[2]; plane++)
        for (uint32_t row = 0; row < size[1]; row++)
            for (uint32_t xB = first; xB < end;) {
                const uint32_t run = std::min(end, (xB & ~15u) + 16) - xB;
                move(volumeOffset(c, l, xB, origin[1] + row, origin[2] + plane), plane, row, xB - first, run);
                xB += run;
            }
}

}  // namespace

bool volumeLayout(uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, uint32_t bpp, bool view2d,
                  MipChain &out)
{
    out = MipChain();
    if (!width || !height || !depth || width > 16384 || height > 16384 || depth > 16384 || !levels ||
        levels > kMaxLevels || levels > fullLevels(std::max(width, depth), height) || !tgm_layout_tiles_bpp(bpp))
        return false;
    out.width = width;
    out.height = height;
    out.depth = depth;
    out.levels = levels;
    out.bpp = bpp;
    const uint8_t z = view2d ? 0 : uint8_t(std::min<uint32_t>(chooseZLog2(depth), tgm_layout_log2_ceil(depth)));
    out.zLog2 = z;
    uint64_t at = 0;
    for (uint32_t l = 0; l < levels; l++) {
        MipLevel &lv = out.level[l];
        lv.width = std::max(1u, width >> l);
        lv.height = std::max(1u, height >> l);
        lv.depth = std::max(1u, depth >> l);
        lv.zLog2 = uint8_t(std::min<uint32_t>(z, tgm_layout_log2_ceil(lv.depth)));
        lv.rowBytes = (lv.width * bpp + 63) & ~63u;
        lv.rows = (lv.height + 7) & ~7u;
        const uint32_t planesPerBlock = 1u << lv.zLog2;
        lv.planes = (lv.depth + planesPerBlock - 1) / planesPerBlock * planesPerBlock;
        lv.offset = at;
        at += uint64_t(lv.rowBytes) * lv.rows * lv.planes;
    }
    const uint64_t tile0 = 512ull << z;
    at = (at + tile0 - 1) / tile0 * tile0;
    out.arrayStride = at;
    out.align = std::max<uint64_t>(tile0, kChainAlign);
    out.bytes = (at + out.align - 1) / out.align * out.align;
    return true;
}

bool msLayout(uint32_t width, uint32_t height, uint32_t samples, uint32_t bpp, MipChain &out)
{
    // NIL SampleLayout::choose_sample_layout, px_extent_sa, and the TEXHEAD MULTI_SAMPLE_COUNT of each (clb097tex.h)
    struct Grid {
        uint32_t samples;
        uint8_t sx, sy, mode;
    };

    static const Grid grids[] = {{1, 0, 0, 0}, {2, 1, 0, 5}, {4, 1, 1, 2}, {8, 2, 1, 4}};
    const Grid *g = nullptr;
    for (const Grid &k : grids)
        if (k.samples == samples)
            g = &k;
    if (!g || !mipChainLayout(width, height, 1, bpp, out))
        return false;
    MipLevel &lv = out.level[0];
    const uint32_t rowsSa = height << g->sy;
    const uint8_t y = uint8_t(std::min<uint32_t>(tgm_layout_y_log2(height), tgm_layout_log2_ceil((rowsSa + 7) / 8)));
    out.yLog2 = lv.yLog2 = y;
    lv.rowBytes = ((width << g->sx) * bpp + 63) & ~63u;
    lv.rows = (rowsSa + (8u << y) - 1) / (8u << y) * (8u << y);
    const uint64_t tile0 = 512ull << y;
    out.arrayStride = (uint64_t(lv.rowBytes) * lv.rows + tile0 - 1) / tile0 * tile0;
    out.align = std::max<uint64_t>(tile0, kChainAlign);
    out.bytes = (out.arrayStride + out.align - 1) / out.align * out.align;
    out.samples = uint8_t(samples);
    out.sxLog2 = g->sx;
    out.syLog2 = g->sy;
    out.sampleMode = g->mode;
    return true;
}

bool arrayLayout(uint32_t width, uint32_t height, uint32_t layers, uint32_t levels, uint32_t bpp, MipChain &out)
{
    if (!layers || layers > 2048 || !mipChainLayout(width, height, levels, bpp, out))
        return false;
    out.layers = layers;
    out.bytes = (out.arrayStride * layers + out.align - 1) / out.align * out.align;
    return true;
}

uint64_t volumeOffset(const MipChain &c, uint32_t l, uint32_t xB, uint32_t y, uint32_t z)
{
    const MipLevel &lv = c.level[l];
    const uint32_t blockRows = 8u << lv.yLog2, blockPlanes = 1u << lv.zLog2;
    const uint64_t blocksPerRow = lv.rowBytes / 64, blockRowsPerPlane = lv.rows / blockRows;
    const uint64_t block = (uint64_t(z / blockPlanes) * blockRowsPerPlane + y / blockRows) * blocksPerRow + xB / 64;
    const uint64_t gob = uint64_t(z % blockPlanes) * (blockRows / 8) + (y % blockRows) / 8;
    return lv.offset + block * (512ull << (lv.yLog2 + lv.zLog2)) + gob * 512 + gobOffset(xB, y);
}

void writeVolume(const MipChain &c, uint8_t *cpu, uint32_t l, const uint32_t origin[3], const uint32_t size[3],
                 const uint8_t *src, size_t rowBytes, size_t planeBytes)
{
    eachRun(c, l, origin, size, [&](uint64_t at, uint32_t plane, uint32_t row, uint32_t from, uint32_t run) {
        memcpy(cpu + at, src + plane * planeBytes + row * rowBytes + from, run);
    });
}

void readVolume(const MipChain &c, const uint8_t *cpu, uint32_t l, const uint32_t origin[3], const uint32_t size[3],
                uint8_t *dst, size_t rowBytes, size_t planeBytes)
{
    eachRun(c, l, origin, size, [&](uint64_t at, uint32_t plane, uint32_t row, uint32_t from, uint32_t run) {
        memcpy(dst + plane * planeBytes + row * rowBytes + from, cpu + at, run);
    });
}

bool blockChainLayout(uint32_t width, uint32_t height, uint32_t levels, uint32_t layers, uint32_t blockW,
                      uint32_t blockH, uint32_t bpp, MipChain &out)
{
    out = MipChain();
    if (!width || !height || width > 16384 || height > 16384 || !levels || levels > kMaxLevels ||
        levels > fullLevels(width, height) || !layers || layers > 2048 || !blockW || !blockH ||
        !tgm_layout_tiles_bpp(bpp))
        return false;
    auto elements = [](uint32_t texels, uint32_t level, uint32_t block) {
        return (std::max(1u, texels >> level) + block - 1) / block;
    };
    out.width = elements(width, 0, blockW);
    out.height = elements(height, 0, blockH);
    out.levels = levels;
    out.layers = layers;
    out.bpp = bpp;
    const uint32_t y = std::min<uint32_t>(tgm_layout_y_log2(height), tgm_layout_log2_ceil((out.height + 7) / 8));
    out.yLog2 = uint8_t(y);
    uint64_t at = 0;
    for (uint32_t l = 0; l < levels; l++) {
        MipLevel &lv = out.level[l];
        lv.width = elements(width, l, blockW);
        lv.height = elements(height, l, blockH);
        lv.yLog2 = uint8_t(std::min(y, tgm_layout_log2_ceil((lv.height + 7) / 8)));
        lv.rowBytes = (lv.width * bpp + 63) & ~63u;
        const uint32_t tileRows = 8u << lv.yLog2;
        lv.rows = (lv.height + tileRows - 1) / tileRows * tileRows;
        lv.offset = at;
        at += uint64_t(lv.rowBytes) * lv.rows;
    }
    const uint64_t tile0 = 512ull << y;
    out.arrayStride = (at + tile0 - 1) / tile0 * tile0;
    out.align = std::max<uint64_t>(tile0, kChainAlign);
    out.bytes = (out.arrayStride * layers + out.align - 1) / out.align * out.align;
    return true;
}

namespace {

// Byte offset of 16-byte line `q` (0..3 across) of row `r` (0..7) in a TegraColor GOB (tgm_layout_gob_offset)
constexpr uint32_t gobLine(uint32_t q, uint32_t r)
{
    return (q >> 1) * 256 + (r >> 1) * 64 + (q & 1) * 32 + (r & 1) * 16;
}

// Each GOB of the rectangle: its first byte in memory, the rows [r0, r1) and bytes [b0, b1) of it the rectangle covers, and
// where its first covered byte is in the linear rows
template <class Gob>
void eachGob(const MipChain &c, uint32_t layer, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
             size_t rowBytes, Gob gob)
{
    const MipLevel &lv = c.level[level];
    const uint64_t base = uint64_t(layer) * c.arrayStride + lv.offset, blockBytes = 512ull << lv.yLog2;
    const uint32_t blockGobs = 1u << lv.yLog2, gobsAcross = lv.rowBytes / 64;
    const uint32_t x0 = x * c.bpp, x1 = (x + w) * c.bpp, y1 = y + h;
    for (uint32_t gy = y / 8; gy * 8 < y1; gy++) {
        const uint32_t r0 = std::max(y, gy * 8) - gy * 8, r1 = std::min(y1, gy * 8 + 8) - gy * 8;
        const uint64_t band =
            base + uint64_t(gy / blockGobs) * gobsAcross * blockBytes + uint64_t(gy % blockGobs) * 512;
        for (uint32_t gx = x0 / 64; gx * 64 < x1; gx++) {
            const uint32_t b0 = std::max(x0, gx * 64) - gx * 64, b1 = std::min(x1, gx * 64 + 64) - gx * 64;
            const size_t linear = size_t(gy * 8 + r0 - y) * rowBytes + (gx * 64 + b0 - x0);
            gob(band + uint64_t(gx) * blockBytes, r0, r1, b0, b1, linear);
        }
    }
}

// One GOB's covered part: all 32 lines at fixed offsets where it is whole, else line by line, each line's covered bytes
template <bool kIn>
void moveGob(uint8_t *gob, uint8_t *linear, size_t rowBytes, uint32_t r0, uint32_t r1, uint32_t b0, uint32_t b1)
{
    if (r0 == 0 && r1 == 8 && b0 == 0 && b1 == 64) {
        for (uint32_t r = 0; r < 8; r++)
            for (uint32_t q = 0; q < 4; q++) {
                uint8_t *line = gob + gobLine(q, r), *row = linear + r * rowBytes + q * 16;
                kIn ? memcpy(line, row, 16) : memcpy(row, line, 16);
            }
        return;
    }
    for (uint32_t r = r0; r < r1; r++)
        for (uint32_t q = b0 / 16; q * 16 < b1; q++) {
            const uint32_t from = std::max(b0, q * 16), to = std::min(b1, q * 16 + 16);
            uint8_t *line = gob + gobLine(q, r) + (from - q * 16), *row = linear + (r - r0) * rowBytes + (from - b0);
            kIn ? memcpy(line, row, to - from) : memcpy(row, line, to - from);
        }
}

}  // namespace

void writeElements(const MipChain &c, uint8_t *memory, uint32_t layer, uint32_t level, uint32_t x, uint32_t y,
                   uint32_t w, uint32_t h, const uint8_t *src, size_t rowBytes)
{
    eachGob(c, layer, level, x, y, w, h, rowBytes,
            [&](uint64_t at, uint32_t r0, uint32_t r1, uint32_t b0, uint32_t b1, size_t linear) {
                moveGob<true>(memory + at, const_cast<uint8_t *>(src) + linear, rowBytes, r0, r1, b0, b1);
            });
}

void readElements(const MipChain &c, const uint8_t *memory, uint32_t layer, uint32_t level, uint32_t x, uint32_t y,
                  uint32_t w, uint32_t h, uint8_t *dst, size_t rowBytes)
{
    eachGob(c, layer, level, x, y, w, h, rowBytes,
            [&](uint64_t at, uint32_t r0, uint32_t r1, uint32_t b0, uint32_t b1, size_t linear) {
                moveGob<false>(const_cast<uint8_t *>(memory) + at, dst + linear, rowBytes, r0, r1, b0, b1);
            });
}

}  // namespace fx
}  // namespace raw
