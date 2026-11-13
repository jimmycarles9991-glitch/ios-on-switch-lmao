// SPDX-License-Identifier: MIT
// Copyright © 2025 Valve Corporation (zcullRegion: Mesa src/nouveau/nil/zcull.rs, nil_zcull_new)
// Copyright © 2022 Collabora Ltd. and Red Hat Inc. (zcullRegionWords: Mesa src/nouveau/vulkan/nvk_cmd_draw.c,
// nvk_CmdBeginRendering)
//
// Zcull, the GPU's coarse depth rejection, for the raw path's passes. The kernel reports its layout (TegraGM20BZcullABI.h)
// and gives every channel a zcull image of its own, so a region holds across context switches. A pass gets one when it
// has a one-sample depth target it clears at its start, as NVK does without a zcull plane: the clear sets the region,
// which a loaded depth's would not match. Its direction is the pass's first depth-testing state's (LESS and LEQUAL less,
// GREATER and GEQUAL greater; NVK, which has no such state at its begin, keeps LESS), set in the pass's prologue, which
// is written once its draws are known.
//
// A draw whose state zcull cannot keep in step with invalidates the region for the rest of the pass (INVALIDATE_ZCULL),
// decided as the depth/stencil state or the program changes, never per draw:
//  - a depth write under ALWAYS or NOT_EQUAL, or a depth test or write of the other direction;
//  - a stencil test whose fail or depth-fail operation writes: a fragment zcull rejects would skip it;
//  - a program that writes depth (its output is not the depth zcull tested), or the shader's emulated tests.
// Discard needs nothing: zcull rejects only fragments the depth test would fail, and the region follows the depth the ROP
// writes. NVK's sources gate none of these per draw; they are the hardware's documented zcull conditions mapped to the
// states raw draws, kept conservative where the GM20B's behaviour is unmeasured.
#pragma once
#include <cstdint>
#include "TegraGM20BZcullABI.h"
#include "raw_kernel.h"
#include "raw_push.h"
#include "raw_zeta.h"

namespace raw {

constexpr uint32_t kZcullMaxSubregions = 16;
constexpr uint32_t kZcullRegionWords = 64;
constexpr uint8_t kZcullLess = 1, kZcullGreater = 2;

inline int Kernel::zcullInfo(TegraGM20BAbiZcullInfo &out)
{
    return call(TegraGM20BMethodZcullInfo, nullptr, 0, &out, sizeof out);
}

struct ZcullRegion {
    uint32_t x = 0, y = 0, width = 0, height = 0;
    uint32_t normalizedAliquots = 0, aliquotCount = 0;
    uint32_t subregions[kZcullMaxSubregions] = {};
    uint32_t subregionCount = 0;
    uint32_t algorithm = 0;  // ASSIGN_ZCULL_SUBREGIONS: 0 static, 1 adaptive
};

inline uint64_t zcullCeil(uint64_t a, uint64_t b)
{
    return (a + b - 1) / b;
}

// nil_zcull_new: the region over [x, x + width) x [y, y + height), its subregions' aliquots and formats from the
// densest that fits the GPU's zcull RAM. False: a layout this cannot take.
inline bool zcullRegion(const TegraGM20BAbiZcullInfo &dev, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                        ZcullRegion &out)
{
    out = ZcullRegion();
    const uint32_t xAlign = dev.subregion_width_align_pixels, yAlign = dev.subregion_height_align_pixels;
    if (!xAlign || !yAlign || !dev.pixel_squares_by_aliquots || !dev.subregion_count ||
        dev.subregion_count > kZcullMaxSubregions || !width || !height)
        return false;
    const uint32_t x0 = x / xAlign * xAlign, y0 = y / yAlign * yAlign;
    const uint32_t x1 = uint32_t(zcullCeil(x + width, xAlign) * xAlign);
    const uint32_t y1 = uint32_t(zcullCeil(y + height, yAlign) * yAlign);
    out.x = x0;
    out.y = y0;
    out.width = x1 - x0;
    out.height = y1 - y0;
    const uint64_t perRegion =
        zcullCeil(zcullCeil(uint64_t(out.width) * out.height, dev.pixel_squares_by_aliquots), dev.subregion_count);
    out.normalizedAliquots = uint32_t(perRegion * dev.subregion_count);
    // SET_ZCULL_SUBREGION_ALLOCATION formats Z_4X8_2X2, Z_8X8_4X2, Z_16X16_4X2, Z_16X16X2_4X4 and the aliquots each takes
    static constexpr uint32_t kFormats[4][2] = {{8, 8}, {5, 4}, {2, 2}, {0, 1}};
    constexpr uint32_t kNone = 0xf;
    uint32_t formats[kZcullMaxSubregions], aliquots[kZcullMaxSubregions] = {};
    uint64_t remaining = dev.aliquot_total;
    for (uint32_t i = dev.subregion_count; i-- > 0;) {
        formats[i] = kNone;
        for (const auto &[format, factor] : kFormats) {
            const uint64_t count = zcullCeil(factor * perRegion, 2);
            if ((i + 1) * count <= remaining) {
                aliquots[i] = uint32_t(count);
                formats[i] = format;
                remaining -= count;
                break;
            }
        }
        out.subregions[i] = i | (aliquots[i] & 0xffff) << 8 | formats[i] << 24;
        out.aliquotCount += aliquots[i];
    }
    for (uint32_t i = 1; i < dev.subregion_count; i++)
        out.algorithm |= formats[i] != formats[0];
    out.subregionCount = dev.subregion_count;
    return out.aliquotCount <= dev.aliquot_total && out.aliquotCount;
}

// SET_ZCULL_DIR_FORMAT's ZFORMAT of a depth format, as NVK takes it (nvk_vk_format_to_zcull_format)
inline uint32_t zcullZFormat(uint32_t metal)
{
    return metal == 250 ? 1u : 3u;  // Depth16Unorm: FP; Depth32Float and _Stencil8: ZF32_1
}

// nvk_CmdBeginRendering's zcull region (no storage: a cleared depth), its direction `dir`
inline void zcullRegionWords(Push &p, const ZcullRegion &r, uint32_t zetaFormat, uint8_t dir)
{
    p.m(0x1590, 0);                                           // SET_ACTIVE_ZCULL_REGION
    p.inc(0x07e0, {r.aliquotCount << 16, r.aliquotCount});    // SET_ZCULL_REGION_LOCATION, _ALIQUOTS
    p.inc(0x07e8, {0, 0, 0, 0});                              // SET_ZCULL_STORAGE_A..D
    p.m(0x15c8, 0);                                           // SET_ZCULL_REGION_FORMAT Z_4X4
    p.inc(0x07c0, {r.width, r.height, 1, 0});                 // SET_ZCULL_REGION_SIZE_A..C, _PIXEL_OFFSET_C
    p.inc(0x15fc, {r.x, r.y});                                // SET_ZCULL_REGION_PIXEL_OFFSET_A, _B
    p.m(0x02e8, 1 | (r.normalizedAliquots & 0xffffff) << 4);  // SET_ZCULL_SUBREGION
    p.m(0x0dd8, 0);                                           // SET_ZCULL_CRITERION: SFUNC_NEVER
    p.m(0x0dbc, (dir == kZcullGreater ? 1u : 0u) | zcullZFormat(zetaFormat) << 16);  // SET_ZCULL_DIR_FORMAT
    p.nonInc(0x02f8, r.subregions, r.subregionCount);                                // SET_ZCULL_SUBREGION_ALLOCATION
    p.m(0x02fc, r.algorithm);                                                        // ASSIGN_ZCULL_SUBREGIONS
    p.m(0x0370, 1);  // SET_ZCULL_SUBREGION_REPORT_TYPE: DEPTH_TEST
}

inline void zcullOffWords(Push &p)
{
    p.m(0x1590, 0x3f);  // SET_ACTIVE_ZCULL_REGION: none
}

constexpr uint32_t kInvalidateZcull = 0x1958;

// A pass's zcull: whether it takes a region, the direction its depth tests go, and whether a draw already invalidated it
struct ZcullPass {
    bool candidate = false;
    uint8_t dir = 0;
    bool invalidated = false;
};

inline uint8_t zcullDirection(uint8_t compare)
{
    switch (compare) {  // MTLCompareFunction
    case 1:             // Less
    case 3:             // LessEqual
        return kZcullLess;
    case 4:  // Greater
    case 6:  // GreaterEqual
        return kZcullGreater;
    default:
        return 0;
    }
}

inline bool zcullStencilWrites(const StencilFace &s)
{
    return (s.compare != 7 && s.fail != 0) || s.depthFail != 0;  // a test that can fail, Keep
}

// Whether INVALIDATE_ZCULL goes before a draw under depth/stencil state `ds` (the pass's targets have depth `depth` and
// stencil `stencil`) of a program that writes depth (`fragmentDepth`) or under the shader's tests; once per pass
inline bool zcullInvalidates(ZcullPass &z, const DepthStencilDesc &ds, bool depth, bool stencil, bool fragmentDepth,
                             bool shaderTests)
{
    if (!z.candidate || z.invalidated)
        return false;
    bool unsafe = fragmentDepth || shaderTests;
    if (stencil)
        unsafe |= zcullStencilWrites(ds.front) || zcullStencilWrites(ds.back);
    if (depth) {
        unsafe |= ds.depthWrite && (ds.depthCompare == 7 || ds.depthCompare == 5);  // Always, NotEqual
        if (const uint8_t dir = zcullDirection(ds.depthCompare)) {
            unsafe |= z.dir && z.dir != dir;
            z.dir = z.dir ? z.dir : dir;
        }
    }
    z.invalidated = unsafe;
    return unsafe;
}

}  // namespace raw
