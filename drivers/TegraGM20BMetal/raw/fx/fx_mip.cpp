// SPDX-License-Identifier: MIT
// See fx_mip.h. Layout rules: Mesa NIL (src/nouveau/nil/image.rs, tiling.rs, copy.rs; MIT); texture header: NIL descriptor.rs
// nvb097_fill_image_view_desc (MIT) with field positions from clb097tex.h; 2D engine methods: cl902d.h and Mesa nvc0_surface.c
// nvc0_2d_texture_set / nvc0_blit_eng2d, nvc0_screen.c's 2D init (MIT).
#include "fx_mip.h"

#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace raw {
namespace fx {
namespace {

// FERMI_TWOD_A (cl902d.h)
enum : uint32_t {
    TWOD_WAIT_FOR_IDLE = NV902D_WAIT_FOR_IDLE,
    TWOD_SET_DST_FORMAT =
        NV902D_SET_DST_FORMAT,  // +4 MEMORY_LAYOUT, +8 BLOCK_SIZE, +c DEPTH, +10 LAYER, +14 PITCH, +18 WIDTH, +1c HEIGHT,
                                // +20 OFFSET_UPPER, +24 OFFSET_LOWER
    TWOD_SET_SRC_FORMAT =
        NV902D_SET_SRC_FORMAT,  // +4 MEMORY_LAYOUT, +8 BLOCK_SIZE, +c DEPTH, (+10 is TWOD_INVALIDATE_TEXTURE_DATA_CACHE),
                                // +14 PITCH, +18 WIDTH, +1c HEIGHT, +20 OFFSET_UPPER, +24 OFFSET_LOWER
    TWOD_INVALIDATE_TEXTURE_DATA_CACHE = NV902D_TWOD_INVALIDATE_TEXTURE_DATA_CACHE,
    TWOD_SET_NUM_PROCESSING_CLUSTERS = NV902D_SET_NUM_PROCESSING_CLUSTERS,
    TWOD_SET_RENDER_ENABLE_C = NV902D_SET_RENDER_ENABLE_C,
    TWOD_SET_CLIP_ENABLE = NV902D_SET_CLIP_ENABLE,
    TWOD_SET_COLOR_KEY_ENABLE = NV902D_SET_COLOR_KEY_ENABLE,
    TWOD_SET_OPERATION = NV902D_SET_OPERATION,
    TWOD_SET_DST_COLOR_RENDER_TO_ZETA_SURFACE = NV902D_SET_DST_COLOR_RENDER_TO_ZETA_SURFACE,
    TWOD_SET_PIXELS_FROM_MEMORY_CORRAL_SIZE = NV902D_SET_PIXELS_FROM_MEMORY_CORRAL_SIZE,
    TWOD_SET_PIXELS_FROM_MEMORY_SAFE_OVERLAP = NV902D_SET_PIXELS_FROM_MEMORY_SAFE_OVERLAP,
    TWOD_SET_PIXELS_FROM_MEMORY_SAMPLE_MODE = NV902D_SET_PIXELS_FROM_MEMORY_SAMPLE_MODE,
    TWOD_SET_PIXELS_FROM_MEMORY_DST_X0 = NV902D_SET_PIXELS_FROM_MEMORY_DST_X0,  // X0, Y0, WIDTH, HEIGHT
    TWOD_SET_PIXELS_FROM_MEMORY_DU_DX_FRAC =
        NV902D_SET_PIXELS_FROM_MEMORY_DU_DX_FRAC,  // DU_DX_FRAC, DU_DX_INT, DV_DY_FRAC, DV_DY_INT
    TWOD_SET_PIXELS_FROM_MEMORY_SRC_X0_FRAC =
        NV902D_SET_PIXELS_FROM_MEMORY_SRC_X0_FRAC,  // SRC_X0_FRAC, SRC_X0_INT, SRC_Y0_FRAC, SRC_Y0_INT (the launch)
};

enum : uint32_t {
    LAYOUT_BLOCKLINEAR = NV902D_SET_DST_MEMORY_LAYOUT_V_BLOCKLINEAR,
    LAYOUT_PITCH = NV902D_SET_DST_MEMORY_LAYOUT_V_PITCH
};

enum : uint32_t {
    SAMPLE_CENTER = NVDEF(NV902D, SET_PIXELS_FROM_MEMORY_SAMPLE_MODE, ORIGIN, CENTER),
    FILTER_POINT = NVDEF(NV902D, SET_PIXELS_FROM_MEMORY_SAMPLE_MODE, FILTER, POINT),
    FILTER_BILINEAR = NVDEF(NV902D, SET_PIXELS_FROM_MEMORY_SAMPLE_MODE, FILTER, BILINEAR)
};

enum : uint32_t {
    OPERATION_SRCCOPY = NV902D_SET_OPERATION_V_SRCCOPY,
    RENDER_ENABLE_TRUE = NV902D_SET_RENDER_ENABLE_C_MODE_TRUE,
    TEXCACHE_L1 = NVDEF(NV902D, TWOD_INVALIDATE_TEXTURE_DATA_CACHE, V, L1_ONLY)
};

// MAXWELL_B (clb197.h), as raw_gpu.cpp names them
enum : uint32_t {
    M3D_WAIT_FOR_IDLE = NVB197_WAIT_FOR_IDLE,
    M3D_INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI = NVB197_INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI
};

constexpr uint32_t kSubc2D = SUBC_2D, kSubc3D = SUBC_3D;

// bits [lo, hi] of the 256-bit header (raw_gpu.cpp setField)
void setField(uint32_t *w, unsigned hi, unsigned lo, uint64_t v)
{
    for (unsigned b = lo; b <= hi; b++, v >>= 1) {
        uint32_t &word = w[b / 32];
        const uint32_t bit = 1u << (b % 32);
        word = (v & 1) ? (word | bit) : (word & ~bit);
    }
}

// A 2D engine surface: SET_{DST,SRC}_* from `base`, written as nvc0_2d_texture_set does (block linear: format, layout, block
// size, depth, then width, height, address; pitch: format, layout, then pitch, width, height, address). The source's block
// linear form stops at DEPTH: nvc0 writes a fifth word there, which lands on TWOD_INVALIDATE_TEXTURE_DATA_CACHE; this writes that
// invalidate on purpose, where it is needed (emitStep).
void surface(Push &p, uint32_t base, uint32_t fmt, bool blockLinear, uint32_t yLog2, uint32_t pitch, uint32_t w,
             uint32_t h, uint64_t va)
{
    if (blockLinear) {
        p.inc(base, {fmt, LAYOUT_BLOCKLINEAR, yLog2 << 4, 1}, kSubc2D);
        if (base == TWOD_SET_DST_FORMAT)
            p.m(base + 0x10, 0, kSubc2D);  // DST_LAYER
        p.inc(base + 0x18, {w, h, uint32_t(va >> 32), uint32_t(va)}, kSubc2D);
    } else {
        p.inc(base, {fmt, LAYOUT_PITCH}, kSubc2D);
        p.inc(base + 0x14, {pitch, w, h, uint32_t(va >> 32), uint32_t(va)}, kSubc2D);
    }
}

constexpr size_t kSurfaceWords = 1 + 4 + 2 + 1 + 4;  // the larger (block-linear destination) form

// One blit: the whole destination rectangle (dx, dy, w, h) from source rectangle origin (sx, sy) with steps du, dv (32.32), the
// source invalidated in the texture data cache first (the 2D engine reads its source through it: a level the step before wrote)
void blit(Push &p, uint32_t filter, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h, uint64_t du, uint64_t dv,
          uint64_t sx, uint64_t sy, uint32_t origin = SAMPLE_CENTER)
{
    p.m(TWOD_INVALIDATE_TEXTURE_DATA_CACHE, TEXCACHE_L1, kSubc2D);
    p.m(TWOD_SET_PIXELS_FROM_MEMORY_SAMPLE_MODE, origin | filter, kSubc2D);
    p.inc(TWOD_SET_PIXELS_FROM_MEMORY_DST_X0, {dx, dy, w, h}, kSubc2D);
    p.inc(TWOD_SET_PIXELS_FROM_MEMORY_DU_DX_FRAC, {uint32_t(du), uint32_t(du >> 32), uint32_t(dv), uint32_t(dv >> 32)},
          kSubc2D);
    p.inc(TWOD_SET_PIXELS_FROM_MEMORY_SRC_X0_FRAC, {uint32_t(sx), uint32_t(sx >> 32), uint32_t(sy), uint32_t(sy >> 32)},
          kSubc2D);
}

constexpr size_t kBlitWords = 1 + 1 + 5 + 5 + 5;

void begin(Push &p)
{
    p.m(M3D_WAIT_FOR_IDLE, 0, kSubc3D);  // the 3D or copy work before wrote the source
    p.m(TWOD_SET_DST_COLOR_RENDER_TO_ZETA_SURFACE, 0, kSubc2D);
}

void end(Push &p)
{
    p.m(M3D_WAIT_FOR_IDLE, 0, kSubc3D);  // the 2D writes are done before draws sample them
    p.m(M3D_INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0, kSubc3D);
}

constexpr size_t kBeginWords = 2, kEndWords = 2, kStepWaitWords = 1;

uint64_t ratio(uint32_t from, uint32_t to)
{
    return (uint64_t(from) << 32) / to;
}

// The source origin of a scaled step of `step` (32.32) as `sampling` reads it, two's complement where it is negative
uint64_t origin(const Sampling &sampling, uint64_t step)
{
    const int64_t half = int64_t(1) << 31;
    return uint64_t(int64_t(sampling.halfDu) * int64_t(step / 2) + int64_t(sampling.halfTexel) * half);
}

}  // namespace

uint32_t fullLevels(uint32_t width, uint32_t height)
{
    uint32_t m = std::max(width, height), l = 1;
    while (m > 1) {
        m >>= 1;
        l++;
    }
    return l;
}

bool mipChainLayout(uint32_t width, uint32_t height, uint32_t levels, uint32_t bpp, MipChain &out)
{
    out = MipChain();
    if (!width || !height || width > 16384 || height > 16384 || !levels || levels > kMaxLevels ||
        levels > fullLevels(width, height) || !tgm_layout_tiles_bpp(bpp))
        return false;
    out.width = width;
    out.height = height;
    out.levels = levels;
    out.bpp = bpp;
    const uint32_t y = tgm_layout_y_log2(height);
    out.yLog2 = uint8_t(y);
    uint64_t at = 0;
    for (uint32_t l = 0; l < levels; l++) {
        MipLevel &lv = out.level[l];
        lv.width = std::max(1u, width >> l);
        lv.height = std::max(1u, height >> l);
        // the level's tiling: level 0's clamped to the level (Tiling::clamp; the hardware does the same at each LOD)
        lv.yLog2 = uint8_t(std::min(y, tgm_layout_log2_ceil((lv.height + 7) / 8)));
        lv.rowBytes = (lv.width * bpp + 63) & ~63u;  // GOB width 64 B, one GOB a tile
        const uint32_t tileRows = 8u << lv.yLog2;
        lv.rows = (lv.height + tileRows - 1) / tileRows * tileRows;
        lv.offset = at;
        at += uint64_t(lv.rowBytes) * lv.rows;
    }
    // array stride rounded to level 0's tile, then the image's alignment: max(level-0 tile bytes, 4096)
    const uint64_t tile0 = 512ull << y;
    at = (at + tile0 - 1) / tile0 * tile0;
    out.arrayStride = at;
    out.align = std::max<uint64_t>(tile0, kChainAlign);
    out.bytes = (at + out.align - 1) / out.align * out.align;
    return true;
}

uint32_t gobOffsetFermi(uint32_t xB, uint32_t y)
{
    // nil/copy.rs CopyGOBFermi::for_each_gob_line, inverted: the 16-byte line of (16-byte column, row) in each 32-byte half
    static const uint8_t line[2][8] = {{0x20, 0x30, 0x00, 0x10, 0xe0, 0xf0, 0xc0, 0xd0},
                                       {0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xa0, 0xb0}};
    return ((xB % 64) / 32) * 0x100 + line[(xB % 32) / 16][y % 8] + (xB % 16);
}

uint64_t texelOffset(const MipChain &c, uint32_t l, uint32_t xB, uint32_t y, bool fermi)
{
    const MipLevel &lv = c.level[l];
    const uint32_t blockRows = 8u << lv.yLog2, gobsPerRow = lv.rowBytes / 64;
    const uint32_t by = y / blockRows, gy = (y % blockRows) / 8;
    const uint64_t block = uint64_t(by) * gobsPerRow + xB / 64;
    return lv.offset + block * (512ull << lv.yLog2) + uint64_t(gy) * 512 +
           (fermi ? gobOffsetFermi(xB, y) : gobOffset(xB, y));
}

namespace {
template <class Move>
void eachRun(const MipChain &c, uint32_t l, uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool fermi, Move move)
{
    const uint32_t bpp = c.bpp, first = x * bpp, end = (x + w) * bpp;
    for (uint32_t row = 0; row < h; row++)
        for (uint32_t xB = first; xB < end;) {
            const uint32_t run = std::min(end, (xB & ~15u) + 16) - xB;
            move(texelOffset(c, l, xB, y + row, fermi), size_t(row), xB - first, run);
            xB += run;
        }
}
}  // namespace

void writeLevel(const MipChain &c, uint8_t *chainCpu, uint32_t l, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                const uint8_t *src, size_t rowBytes, bool fermi)
{
    eachRun(c, l, x, y, w, h, fermi, [&](uint64_t at, size_t row, uint32_t from, uint32_t run) {
        memcpy(chainCpu + at, src + row * rowBytes + from, run);
    });
}

void readLevel(const MipChain &c, const uint8_t *chainCpu, uint32_t l, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
               uint8_t *dst, size_t rowBytes, bool fermi)
{
    eachRun(c, l, x, y, w, h, fermi, [&](uint64_t at, size_t row, uint32_t from, uint32_t run) {
        memcpy(dst + row * rowBytes + from, chainCpu + at, run);
    });
}

void mipChainTic(const MipChain &c, uint64_t va, uint32_t tic0, bool srgb, uint32_t baseLevel, uint32_t levelCount,
                 uint32_t out[8], uint32_t textureType)
{
    memset(out, 0, 32);
    out[0] = tic0;                                // COMPONENTS, data types, X..W_SOURCE (NIL nvb097_set_th_bl_0)
    setField(out, 63, 41, (va >> 9) & 0x7fffff);  // ADDRESS_BITS31TO9
    setField(out, 79, 64, (va >> 32) & 0xffff);   // ADDRESS_BITS47TO32
    setField(out, 87, 85, 3);                     // HEADER_VERSION SELECT_BLOCKLINEAR
    setField(out, 98, 96, 0);                     // GOBS_PER_BLOCK_WIDTH ONE_GOB
    setField(out, 101, 99, c.yLog2);              // GOBS_PER_BLOCK_HEIGHT (level 0's)
    setField(out, 104, 102, c.zLog2);             // GOBS_PER_BLOCK_DEPTH
    setField(out, 108, 106, 0);                   // TILE_WIDTH_IN_GOBS
    setField(out, 112, 112, 1);                   // LOD_ANISO_QUALITY2
    setField(out, 113, 113, 1);                   // LOD_ANISO_QUALITY HIGH
    setField(out, 114, 114, 1);                   // LOD_ISO_QUALITY HIGH
    setField(out, 116, 115, 0);                   // ANISO_COARSE_SPREAD_MODIFIER NONE
    // MAX_MIP_LEVEL: nil_rs_max_mip_level, 0 for a one-level view of level 0 (unnormalized coordinates stay possible), else the
    // image's last level
    const bool single = levelCount == 1 && baseLevel == 0 && textureType != kTexThreeD && c.layers == 1;
    setField(out, 127, 124, single ? 0 : c.levels - 1);
    setField(out, 143, 128, c.width - 1);   // WIDTH_MINUS_ONE
    setField(out, 150, 150, srgb ? 1 : 0);  // S_R_G_B_CONVERSION
    setField(out, 154, 151, textureType);   // TEXTURE_TYPE
    setField(out, 156, 155, 0);             // SECTOR_PROMOTION NO_PROMOTION
    setField(out, 159, 157, 7);             // BORDER_SIZE BORDER_SAMPLER_COLOR
    setField(out, 175, 160, c.height - 1);  // HEIGHT_MINUS_ONE
    // DEPTH_MINUS_ONE: a 3D image's depth, an array's layers, a cube array's cubes
    const bool cube = textureType == kTexCube || textureType == kTexCubeArray;
    setField(out, 189, 176, (textureType == kTexThreeD ? c.depth : cube ? c.layers / 6 : c.layers) - 1);
    setField(out, 191, 191, 1);                           // NORMALIZED_COORDS
    setField(out, 216, 215, 2);                           // ANISO_FINE_SPREAD_FUNC TWO
    setField(out, 218, 217, 1);                           // ANISO_COARSE_SPREAD_FUNC ONE
    setField(out, 227, 224, baseLevel);                   // RES_VIEW_MIN_MIP_LEVEL
    setField(out, 231, 228, baseLevel + levelCount - 1);  // RES_VIEW_MAX_MIP_LEVEL
    setField(out, 235, 232, c.sampleMode);                // MULTI_SAMPLE_COUNT
    setField(out, 247, 236, 0);  // MIN_LOD_CLAMP: min_lod_clamp 0 - base level, clamped at 0 (ufixed 4.8)
}

uint32_t twodFormat(uint32_t mtl)
{
    // formats whose 2D-engine filtering is plain per-component unorm/float arithmetic, as Vulkan's linear blit does, and the
    // sRGB forms, which it decodes before it filters and encodes after, as Metal does: gpu.x1.twod_scale's 2:1 bilinear cells
    // bgra8srgb and rgba8srgb are within one code of the texture unit's linear-space filtering at every texel (637 by one),
    // where filtering the codes would put all 4,096 of them 2-62 codes off. The other formats are the 3D engine's (raw_mip.h).
    switch (mtl) {
    case 81:
        return 0xd0;  // BGRA8Unorm_sRGB -> A8RL8GL8BL8
    case 71:
        return 0xd6;  // RGBA8Unorm_sRGB -> A8BL8GL8RL8
    case 80:
        return 0xcf;  // BGRA8Unorm -> A8R8G8B8
    case 70:
        return 0xd5;  // RGBA8Unorm -> A8B8G8R8
    case 115:
        return 0xca;  // RGBA16Float -> RF16_GF16_BF16_AF16
    case 10:
        return 0xf3;  // R8Unorm -> Y8
    case 30:
        return 0xea;  // RG8Unorm -> G8R8
    default:
        return 0;
    }
}

size_t twodInitWords()
{
    return 7;
}

void emitTwodInit(Push &p)
{
    p.m(TWOD_SET_NUM_PROCESSING_CLUSTERS, 0, kSubc2D);  // ALL (nvc0: SINGLE_GPC 0)
    p.m(TWOD_SET_OPERATION, OPERATION_SRCCOPY, kSubc2D);
    p.m(TWOD_SET_CLIP_ENABLE, 0, kSubc2D);
    p.m(TWOD_SET_COLOR_KEY_ENABLE, 0, kSubc2D);
    p.m(TWOD_SET_PIXELS_FROM_MEMORY_CORRAL_SIZE, 0x3f, kSubc2D);
    p.m(TWOD_SET_PIXELS_FROM_MEMORY_SAFE_OVERLAP, 1, kSubc2D);
    p.m(TWOD_SET_RENDER_ENABLE_C, RENDER_ENABLE_TRUE, kSubc2D);
}

size_t generateWords(const MipChain &c, uint32_t firstLevel, uint32_t lastLevel)
{
    (void)c;
    const size_t steps =
        (lastLevel > firstLevel ? lastLevel - firstLevel : 0) + 1;  // + the level-0 conversion, at most
    return kBeginWords + kEndWords + steps * (2 * kSurfaceWords + kBlitWords + kStepWaitWords);
}

void emitGenerate(Push &p, const MipChain &c, uint64_t chainVa, const Pitch *level0, uint32_t fmt2d,
                  uint32_t firstLevel, uint32_t lastLevel, Sampling sampling)
{
    if (!c.levels || lastLevel >= c.levels || firstLevel > lastLevel || (level0 && firstLevel))
        return;
    begin(p);
    if (level0) {  // pitch level 0 -> chain level 0: 1:1, point (a layout conversion, no filtering)
        const MipLevel &d = c.level[0];
        surface(p, TWOD_SET_DST_FORMAT, fmt2d, true, d.yLog2, 0, d.width, d.height, chainVa + d.offset);
        surface(p, TWOD_SET_SRC_FORMAT, fmt2d, false, 0, level0->pitch, level0->width, level0->height, level0->va);
        blit(p, FILTER_POINT, 0, 0, d.width, d.height, 1ull << 32, 1ull << 32, 0, 0);
    }
    for (uint32_t l = firstLevel; l < lastLevel; l++) {
        const MipLevel &s = c.level[l], &d = c.level[l + 1];
        p.m(TWOD_WAIT_FOR_IDLE, 0, kSubc2D);  // level l's writes are done before level l + 1 reads them
        surface(p, TWOD_SET_DST_FORMAT, fmt2d, true, d.yLog2, 0, d.width, d.height, chainVa + d.offset);
        surface(p, TWOD_SET_SRC_FORMAT, fmt2d, true, s.yLog2, 0, s.width, s.height, chainVa + s.offset);
        const uint64_t du = ratio(s.width, d.width), dv = ratio(s.height, d.height);
        blit(p, FILTER_BILINEAR, 0, 0, d.width, d.height, du, dv, origin(sampling, du), origin(sampling, dv),
             sampling.corner ? 1u : SAMPLE_CENTER);
    }
    end(p);
}

size_t copyWords()
{
    return kBeginWords + kEndWords + 2 * kSurfaceWords + kBlitWords;
}

void emitCopyToLevel(Push &p, const MipChain &c, uint64_t chainVa, uint32_t level, const Pitch &src, uint32_t fmt2d,
                     uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h)
{
    if (level >= c.levels || !w || !h)
        return;
    const MipLevel &d = c.level[level];
    if (dx + w > d.width || dy + h > d.height || sx + w > src.width || sy + h > src.height)
        return;
    begin(p);
    surface(p, TWOD_SET_DST_FORMAT, fmt2d, true, d.yLog2, 0, d.width, d.height, chainVa + d.offset);
    surface(p, TWOD_SET_SRC_FORMAT, fmt2d, false, 0, src.pitch, src.width, src.height, src.va);
    blit(p, FILTER_POINT, dx, dy, w, h, 1ull << 32, 1ull << 32, uint64_t(sx) << 32, uint64_t(sy) << 32);
    end(p);
}

// ---- reference ----

void referenceDownsample(const uint8_t *src, uint32_t srcPitch, uint32_t sw, uint32_t sh, uint8_t *dst,
                         uint32_t dstPitch, uint32_t dw, uint32_t dh, uint32_t bpp)
{
    // the sample position of destination texel x: (x + 0.5) * sw / dw in source texels, then bilinear between the two nearest
    // texel centres (at i + 0.5), clamped to the edge; weights quantized to 1/256 as the texture filter does
    auto axis = [](uint32_t x, uint32_t s, uint32_t d, uint32_t &i0, uint32_t &i1, uint32_t &w1) {
        const double u = (x + 0.5) * double(s) / double(d) - 0.5;
        const double f = std::floor(u);
        const int a = int(f);
        w1 = uint32_t(std::lround((u - f) * 256.0));
        i0 = uint32_t(std::clamp(a, 0, int(s) - 1));
        i1 = uint32_t(std::clamp(a + 1, 0, int(s) - 1));
        if (w1 == 256) {  // rounds up to the next texel
            i0 = i1;
            w1 = 0;
        }
    };
    for (uint32_t y = 0; y < dh; y++) {
        uint32_t y0, y1, wy;
        axis(y, sh, dh, y0, y1, wy);
        for (uint32_t x = 0; x < dw; x++) {
            uint32_t x0, x1, wx;
            axis(x, sw, dw, x0, x1, wx);
            for (uint32_t k = 0; k < bpp; k++) {
                const uint32_t a = src[size_t(y0) * srcPitch + x0 * bpp + k],
                               b = src[size_t(y0) * srcPitch + x1 * bpp + k];
                const uint32_t c = src[size_t(y1) * srcPitch + x0 * bpp + k],
                               d = src[size_t(y1) * srcPitch + x1 * bpp + k];
                const uint32_t top = a * (256 - wx) + b * wx, bot = c * (256 - wx) + d * wx;
                const uint32_t v = top * (256 - wy) + bot * wy;  // 8.16
                dst[size_t(y) * dstPitch + x * bpp + k] = uint8_t((v + 32768) >> 16);
            }
        }
    }
}

}  // namespace fx
}  // namespace raw
