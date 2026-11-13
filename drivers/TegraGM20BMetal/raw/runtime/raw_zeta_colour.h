// SPDX-License-Identifier: MIT
// The one-draw stencil route: a stencil plane written by the colour ROP over the zeta's own memory, as Gallium nvc0 writes
// depth and stencil (nv50_blit.h nv50_blit_zeta_to_colour_format and nv50_blit_derive_color_mask, nvc0_surface.c
// nvc0_blit_3d). The zeta is a colour target of its texel size over its own block-linear layout (raw_zeta.h: a zeta's GOBs
// are a colour image's; its PTE kind stays the zeta's), and one draw of the pack's program (pack/stencil_bits.metal's
// *_colour_frag) writes each texel:
//  - Depth32Float_Stencil8 (ZF32_X24S8) as RG32Uint: R its depth's bits, G its stencil in the low byte, the word its stencil
//    view (X32_S8X24_UINT, raw_stencil_bits.h kStencilViews) reads. nvc0 draws R32G32_FLOAT; an integer target stores the
//    words as the program gives them. A turn copy writes both words; a resolve writes G alone (nvc0's XS mode: colour mask
//    G), keeping the depth its depth resolve wrote.
//  - Stencil8 (S8) as R8Uint, the format its own header samples it as. nvc0 has no such zeta.
// Taken where raw_chip_facts.h's kColourRopWritesZeta holds, or while a debug build's probe asks for it (hw_test_app
// gpu.raw.zeta_colour), into a one-sample zeta without comptags (the colour ROP keeps no zeta compression); any other
// stencil plane is raw_stencil_bits.h's eight draws. The draw is a pass of its own: the WAIT_FOR_IDLE of the barrier between
// passes orders its colour writes after the zeta writes before it and before the zeta tests after it, as nvc0 serializes a
// surface it flips between zeta and colour.
#pragma once
#include <cstdint>
#include <type_traits>
#include "raw_stencil_bits.h"

namespace raw {

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

// Whether the one-draw route writes zeta `z` while the route is `taken`
template <typename Texture> bool zetaColourWrites(const Texture &z, bool taken)
{
    return taken && zetaColourFormat(z.format) && z.samples == 1 && !z.comptags;
}

// Zeta `z`'s memory as the route's colour target
template <typename Texture> Texture zetaColourView(const Texture &z)
{
    Texture c = z;
    c.zeta = false;
    c.format = zetaColourFormat(z.format);
    c.renderable = true;
    c.tic = 0;
    return c;
}

// The route's pass into `view` over (0, 0, width, height) (0: the whole target), no zeta bound. Loaded, never DontCare: a
// stored DontCare target written through a partial mask is cleared first (RenderEncoder::clears), which would zero a
// resolve's depth word, and a turn copy keeps the texels outside its source rectangle.
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

}  // namespace raw
