// SPDX-License-Identifier: MIT
// The 2D engine's blit-encoder work outside the probes. Rule resolve.twod (resolve_rule.h), and the probe's
// complement: an MSAA resolve as one 2D-engine scaled blit. The multisampled image is NIL's image of its sample grid
// (fx::msLayout: 2x is 2x1 samples a pixel, 4x 2x2), so the grid read at 2:1 bilinear, corner origin at half a step,
// gives each destination texel the mean of its samples. The blit encoder's opening wait orders the read after the pass
// that rendered the samples, as for every blit.
#include "../../kits/SwitchGM20BEmit/SwitchGM20BEmit2D.h"
#include "fx/fx_image.h"
#include "fx/fx_mip.h"
#include "gpu.h"
#include "kit.h"
#include "resolve_rule.h"
#include "samples.h"
#include "twod_fill.h"

#include <algorithm>

namespace gm20b {

namespace {

switch_gm20b_2d_surface gridSurface(const Texture &t, uint32_t fmt2d, uint32_t width, uint32_t height)
{
    switch_gm20b_2d_surface s = {};
    s.address = t.va;
    s.format = fmt2d;
    s.pitch = !t.blockLinear;
    s.pitch_bytes = t.pitch;
    s.block_height_log2 = t.blockHeight;
    s.width = width;
    s.height = height;
    s.compressed = t.comptags;
    return s;
}

// fx::twodFormat's formats, RGB10A2Unorm, whose resolve the pack has no state for, RG11B10Float, and the signed normalized
// formats as the 2D engine's signed formats: the 16-bit ones resolved as the mean of their codes (attachment_target.h
// codesResolvedByTwod), the 8-bit ones spread alone (spreadSamples)
uint32_t resolveFormat(uint32_t mtl)
{
    switch (mtl) {
    case 12:
        return NV902D_SET_DST_FORMAT_V_RN8;
    case 22:
        return NV902D_SET_DST_FORMAT_V_RN16;
    case 32:
        return NV902D_SET_DST_FORMAT_V_GN8RN8;
    case 62:
        return NV902D_SET_DST_FORMAT_V_RN16_GN16;
    case 72:
        return NV902D_SET_DST_FORMAT_V_AN8BN8GN8RN8;
    case 112:
        return NV902D_SET_DST_FORMAT_V_RN16_GN16_BN16_AN16;
    case 90:
        return NV902D_SET_DST_FORMAT_V_A2B10G10R10;
    case 92:
        return NV902D_SET_DST_FORMAT_V_BF10GF11RF11;
    default:
        return fx::twodFormat(mtl);
    }
}

// For a format with no 2D format of its own: one of `bpp`-byte texels whose point-sampled copy keeps every code, unorm fields
// the engine reads and writes back exactly, 16-byte texels as four floats
uint32_t spreadFormat(uint32_t bpp)
{
    switch (bpp) {
    case 1:
        return NV902D_SET_DST_FORMAT_V_Y8;
    case 2:
        return NV902D_SET_DST_FORMAT_V_Y16;
    case 4:
        return NV902D_SET_DST_FORMAT_V_A8R8G8B8;
    case 8:
        return NV902D_SET_DST_FORMAT_V_R16_G16_B16_A16;
    case 16:
        return NV902D_SET_DST_FORMAT_V_RF32_GF32_BF32_AF32;
    default:
        return 0;
    }
}

}  // namespace

// The complement's ROP writes go through the CROP and the scale reads the grid through the 2D engine's texture path, so
// each is waited for before the next touches the grid, as fx_mip.cpp waits between a chain's levels
void BlitEncoder::resolveTwod(const Texture &ms, uint32_t sxLog2, uint32_t syLog2, const Texture &into, uint32_t width,
                              uint32_t height, uint32_t fmt2d, bool complement, bool keep)
{
    room(SWITCH_GM20B_2D_SCALE_WORDS + 2 * (SWITCH_GM20B_2D_INVERT_WORDS + 2));
    cb_->use(ms, complement);
    cb_->use(into, true);
    const switch_gm20b_2d_surface grid = gridSurface(ms, fmt2d, ms.width << sxLog2, ms.height << syLog2);
    const switch_gm20b_2d_surface out = gridSurface(into, fmt2d, into.width, into.height);
    const uint64_t du = (1ull << 32) << sxLog2, dv = (1ull << 32) << syLog2;
    const uint8_t rop = complement ? SWITCH_GM20B_2D_ROP_NOTSRCCOPY : 0;
    const switch_gm20b_2d_scale_op op = {0, 0, width, height, du, dv, du / 2, dv / 2, true, true, rop, false};
    if (complement) {
        kit(p_, switch_gm20b_2d_invert, &grid, 0, 0, grid.width, grid.height);
        p_.m(NV902D_WAIT_FOR_IDLE, 0, SUBC_2D);
    }
    kit(p_, switch_gm20b_2d_scale, &grid, &out, &op);
    if (complement && keep) {
        p_.m(NV902D_WAIT_FOR_IDLE, 0, SUBC_2D);
        kit(p_, switch_gm20b_2d_invert, &grid, 0, 0, grid.width, grid.height);
    }
}

// Each destination sample of the grid reads the texel of its pixel: point sampled at a 1:2 step, the centre origin putting a
// pixel's two (four) grid texels a quarter step either side of its texel's centre
void BlitEncoder::spreadTwod(const Texture &one, const Texture &ms, uint32_t sxLog2, uint32_t syLog2, uint32_t width,
                             uint32_t height, uint32_t fmt2d)
{
    room(SWITCH_GM20B_2D_SCALE_WORDS);
    cb_->use(one, false);
    cb_->use(ms, true);
    const switch_gm20b_2d_surface from = gridSurface(one, fmt2d, one.width, one.height);
    const switch_gm20b_2d_surface grid = gridSurface(ms, fmt2d, ms.width << sxLog2, ms.height << syLog2);
    const uint64_t du = (1ull << 32) >> sxLog2, dv = (1ull << 32) >> syLog2;
    const switch_gm20b_2d_scale_op op = {0, 0, width << sxLog2, height << syLog2, du, dv, 0, 0, false, false, 0, false};
    kit(p_, switch_gm20b_2d_scale, &from, &grid, &op);
}

void BlitEncoder::fillTwod(const Texture &t, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t word)
{
    room(SWITCH_GM20B_2D_FILL_WORDS);
    cb_->use(t, true);
    const switch_gm20b_2d_surface s = gridSurface(t, twodFillFormat(t.format), t.width, t.height);
    kit(p_, switch_gm20b_2d_fill, &s, word, x0, y0, x1, y1);
}

bool CommandBuffer::resolveTwod(const Texture &multisampled, const Texture &into, bool exact, bool keepSamples,
                                uint32_t width, uint32_t height)
{
    fx::MipChain grid;
    const uint32_t fmt2d = resolveFormat(into.format);
    const bool whole = !width && !height;
    if (!fmt2d || into.format != multisampled.format || into.samples > 1 ||
        !fx::msLayout(multisampled.width, multisampled.height, multisampled.samples, multisampled.bpp, grid) ||
        grid.sxLog2 > 1 || grid.syLog2 > 1 || (!grid.sxLog2 && !grid.syLog2) ||
        (whole && (multisampled.width != into.width || multisampled.height != into.height)))
        return false;
    BlitEncoder *b = blit();
    if (!b)
        return false;
    const uint32_t w = whole ? into.width : std::min({width, multisampled.width, into.width}),
                   h = whole ? into.height : std::min({height, multisampled.height, into.height});
    b->resolveTwod(multisampled, grid.sxLog2, grid.syLog2, into, w, h, fmt2d, exact, keepSamples);
    b->end();
    SWITCH_METAL_DIAGNOSTIC(dev_->stats.resolveTwod++; dev_->stats.resolveExact += exact;
                            dev_->stats.resolveTwodCompressed += multisampled.comptags;
                            dev_->stats.resolveRoute[exact ? ResolveByComplement : ResolveByTwod]
                                                    [resolveSampleSlot(multisampled.samples)]++);
    return true;
}

bool CommandBuffer::spreadSamples(const Texture &resolved, const Texture &multisampled, uint32_t width, uint32_t height)
{
    fx::MipChain grid;
    const uint32_t own = resolveFormat(resolveTargetFormat(multisampled.format));
    const uint32_t fmt2d = own ? own : spreadFormat(multisampled.bpp);
    if (!fmt2d || resolved.format != multisampled.format || resolved.samples > 1 ||
        !fx::msLayout(multisampled.width, multisampled.height, multisampled.samples, multisampled.bpp, grid))
        return false;
    BlitEncoder *b = blit();
    b->spreadTwod(resolved, multisampled, grid.sxLog2, grid.syLog2,
                  std::min({width, resolved.width, multisampled.width}),
                  std::min({height, resolved.height, multisampled.height}), fmt2d);
    b->end();
    return true;
}

}  // namespace gm20b
