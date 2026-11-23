// SPDX-License-Identifier: MIT
// The one-draw stencil route: a stencil plane written by one draw of the colour ROP, as Gallium nvc0 writes depth and
// stencil (nv50_blit.h nv50_blit_zeta_to_colour_format and nv50_blit_derive_color_mask, nvc0_surface.c nvc0_blit_3d). The
// zeta is a colour image of its texel size over its own block-linear layout (zeta.h: a zeta's GOBs are a colour
// image's), and the pack's program (pack/stencil_bits.metal's *_colour_frag) writes each texel:
//  - Depth32Float_Stencil8 (ZF32_X24S8) as RG32Uint: R its depth's bits, G its stencil in the low byte, the word its stencil
//    view (X32_S8X24_UINT, stencil_bits.h kStencilViews) reads. nvc0 draws R32G32_FLOAT; an integer target stores the
//    words as the program gives them. A turn copy writes both words; a resolve writes G alone (nvc0's XS mode: colour mask
//    G), keeping the depth its depth resolve wrote.
//  - Stencil8 (S8) as R8Uint, the format its own header samples it as. nvc0 has no such zeta.
// The colour ROP does not write the zeta's pages: on GM20B its writes through a ZF32_X24S8 or S8 PTE kind never reached
// memory (boot 21, gpu.gm20b.zeta_colour: every route-written texel read back as it was before the route, by the copy engine
// and by the zeta's own tests, with no fault or ROP trap), though nvc0 draws into kind 0xc3 the same way. The view is
// rendered as a shadow target is (zetaPages): through a plain pool image of its colour format that the copy engine fills
// from the zeta before the draw and copies back over the render area after it (CommandBuffer::render, RenderEncoder::end),
// the engine whose zeta writes gpu.gm20b.stored_depth proves on both kinds. The fill keeps the texels the draw leaves: a
// resolve's depth word, a turn copy's texels outside its source rectangle.
// Every one-sample zeta without comptags takes it; any other stencil plane is stencil_bits.h's eight draws, and so is
// every plane while a debug build's probe asks for them (hw_test_app gpu.gm20b.zeta_colour's comparison arm).
#pragma once
#include <cstdint>
#include <type_traits>
#include "stencil_bits.h"

namespace gm20b {

// The colour format zeta `format`'s texels are written as; 0 for none
inline uint32_t zetaColourFormat(uint32_t format)
{
    switch (format) {
    case 260:
        return 103;  // RG32Uint
    case 253:
        return 13;  // R8Uint
    default:
        return 0;
    }
}

// The pack's one-draw state into a zeta of `format` from a source of `samples` (one: the texel at the args' map, depth and
// stencil; several: each pixel's `pick` sample's stencil, Zero and Last the args' fixed one); null for none
inline const char *stencilColourState(uint32_t format, uint32_t samples, StencilSample pick)
{
    const bool s8 = format == 253;
    if (!s8 && format != 260)
        return nullptr;
    if (samples == 1)
        return pick != StencilSample::Zero ? nullptr
               : s8                        ? "raw.stencil_bits.stencil_bits_s8_colour_frag"
                                           : "raw.stencil_bits.stencil_bits_d32fs8_colour_frag";
    switch (pick) {
    case StencilSample::Zero:
    case StencilSample::Last:
        return s8 ? "raw.stencil_bits.stencil_bits_ms_s8_colour_frag"
                  : "raw.stencil_bits.stencil_bits_ms_d32fs8_colour_frag";
    case StencilSample::LeastDepth:
        return s8 ? nullptr : "raw.stencil_bits.stencil_bits_ms_d32fs8_min_colour_frag";
    case StencilSample::GreatestDepth:
        return s8 ? nullptr : "raw.stencil_bits.stencil_bits_ms_d32fs8_max_colour_frag";
    }
    return nullptr;
}

// Whether the one-draw route writes zeta `z`
template <typename Texture> bool zetaColourWrites(const Texture &z)
{
    return zetaColourFormat(z.format) && z.samples == 1 && !z.comptags;
}

// Zeta `z`'s memory as a colour target of `format`, rendered through a pool image
template <typename Texture> Texture zetaColourView(const Texture &z, uint32_t format)
{
    Texture c = z;
    c.zeta = false;
    c.format = format;
    c.renderable = true;
    c.zetaPages = true;
    c.tic = 0;
    return c;
}

// Zeta `z`'s memory as the route's colour target, rendered through a pool image
template <typename Texture> Texture zetaColourView(const Texture &z)
{
    return zetaColourView(z, zetaColourFormat(z.format));
}

// Same-bits attachments, which Metal's validation layer stops and Apple's GPU renders (an M4 Max, validation off,
// c38-audit/m2v-pass-forms/probe/bitsprobe.mm): a one-sample Depth32Float or Depth16Unorm texture as a colour attachment
// takes the R32Float or R16Unorm bits of a clear and of a state of that format; a one-sample colour texture of 4-byte texels
// as the depth attachment holds Depth32Float bits (a Load reads its bytes as the float, a clear and a draw store floats).
// Each renders through a backing of the other class that the copy engine fills from the texture's memory and drains back
// into it: the colour ROP's writes through a zeta's PTE kind never reach memory (above), and the zeta ROP's through a
// colour kind are unmeasured (hw_test_app gpu.gm20b.same_bits). Other texel sizes as depth do not keep their bits: not
// mirrored.
inline uint32_t depthAsColourFormat(uint32_t format)
{
    switch (format) {
    case 252:
        return 55;  // R32Float
    case 250:
        return 20;  // R16Unorm
    default:
        return 0;
    }
}

constexpr uint32_t kColourAsDepthFormat = 252;  // Depth32Float

template <typename Texture> bool colourAsDepth(const Texture &t)
{
    return !t.zeta && !t.converted() && t.bpp == 4 && t.samples <= 1;
}

// The zeta backing's shape of colour texture `t` as the depth attachment
template <typename Texture> Texture colourAsDepthShape(const Texture &t)
{
    Texture z;
    z.width = t.width;
    z.height = t.height;
    z.layers = t.layers;
    z.samples = 1;
    z.format = kColourAsDepthFormat;
    z.zeta = true;
    return z;
}

// The route's pass into `view` over (0, 0, width, height) (0: the whole target), no zeta bound. Loaded, never DontCare: the
// load is the pool image's fill from the zeta, which keeps a resolve's depth word and a turn copy's texels outside its
// source rectangle.
template <typename Pass, typename Texture> Pass zetaColourPass(Texture &view, uint32_t width, uint32_t height)
{
    using Attachment = std::decay_t<decltype(Pass().color[0])>;
    Pass p;
    p.color[0].texture = &view;
    p.color[0].load = Attachment::Load;
    p.color[0].store = true;
    p.count = 1;
    p.width = width;
    p.height = height;
    return p;
}

}  // namespace gm20b
