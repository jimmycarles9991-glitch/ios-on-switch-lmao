// SPDX-License-Identifier: MIT
// A stencil plane the 3D engine writes with its fixed-function stencil, as NVK does through Mesa's vk_meta
// (vk_meta_blit_resolve.c stencil_as_discard): the target's stencil is cleared to 0, then a full-target quad is drawn once a
// bit, the ROP writing reference 0xff through write mask 1 << bit under compare Always and operation Replace, and the pack's
// program (pack/stencil_bits.metal) discarding each fragment whose source stencil has that bit clear.
//  - a turned pass's unturned zeta (raw_turn_copy.h): its stencil moved between frame and plane at a TurnMap;
//  - a multisampled zeta's stencil resolve: sample 0's stencil, or MTLMultisampleStencilResolveFilterDepthResolvedSample's,
//    the stencil of the sample whose depth the depth resolve's filter picks (sample 0, or its least or greatest depth, the
//    first such in sample order) and, where the depth is not resolved, the last sample's (what an M-series Mac's GPU
//    resolves: hw_test_app gpu.raw.stored_msaa on the host).
// The source's stencil is read through the header NVK samples a stencil aspect through (kStencilViews), a multisampled one's
// samples alike (the header's other words are the zeta's own). A plane raw_zeta_colour.h's one draw takes is written by it
// instead.
#pragma once
#include <cstdint>
#include "raw_turn_copy.h"
#include "raw_zeta.h"

namespace raw {

constexpr uint32_t kStencilBits = 8;
constexpr uint32_t kStencilBitReference = 0xff;

// Bit `k`'s draw state: depth neither tested nor written
inline DepthStencilDesc stencilBitState(uint32_t k)
{
    DepthStencilDesc d;
    d.front.compare = 7;  // MTLCompareFunctionAlways
    d.front.pass = 2;     // MTLStencilOperationReplace
    d.front.writeMask = 1u << k;
    d.back = d.front;
    return d;
}

// The sample of a multisampled source whose stencil a resolve takes
enum class StencilSample : uint32_t {
    Zero,
    LeastDepth,
    GreatestDepth,
    Last
};

// MTLMultisampleStencilResolveFilter `filter`'s sample beside a depth attachment (`depth`) whose resolve, where it has one
// (`depthResolves`), is under MTLMultisampleDepthResolveFilter `depthFilter` (Sample0, Min, Max)
inline StencilSample stencilResolveSample(uint32_t filter, uint32_t depthFilter, bool depth, bool depthResolves)
{
    if (filter != 1 /* MTLMultisampleStencilResolveFilterDepthResolvedSample */ || !depth)
        return StencilSample::Zero;
    if (!depthResolves)
        return StencilSample::Last;
    return depthFilter <= 2 ? StencilSample(depthFilter) : StencilSample::Zero;
}

// pack/stencil_bits.metal Bits, the programs' buffer 0: `sample` the one a fixed-sample program reads
struct StencilBitsArgs {
    TurnMap map;
    uint32_t bit = 0, samples = 1, sample = 0;
};

inline StencilBitsArgs stencilBitsArgs(const TurnMap &map, uint32_t samples, StencilSample pick)
{
    StencilBitsArgs a;
    a.map = map;
    a.samples = samples;
    a.sample = pick == StencilSample::Last ? samples - 1 : 0;
    return a;
}

// Each of the draws that write the stencil: `draw(k, args)` with `base` under bit k; false: the draw's answer
template <typename Draw> bool forEachStencilBit(StencilBitsArgs base, Draw draw)
{
    for (uint32_t k = 0; k < kStencilBits; k++) {
        base.bit = 1u << k;
        if (!draw(k, base))
            return false;
    }
    return true;
}

// The pack's stencil bits state into a zeta target of `format` (Stencil8, Depth32Float_Stencil8) from a source of `samples`
// (one: its texel at the args' map; several: each pixel's `pick` sample, Zero and Last the args' fixed one); null for none
inline const char *stencilBitsState(uint32_t format, uint32_t samples, StencilSample pick)
{
    const bool s8 = format == 253;
    if (!s8 && format != 260)
        return nullptr;
    if (samples == 1)
        return pick != StencilSample::Zero ? nullptr
               : s8                        ? "raw.stencil_bits.stencil_bits_s8_frag"
                                           : "raw.stencil_bits.stencil_bits_d32fs8_frag";
    switch (pick) {
    case StencilSample::Zero:
    case StencilSample::Last:
        return s8 ? "raw.stencil_bits.stencil_bits_ms_s8_frag" : "raw.stencil_bits.stencil_bits_ms_d32fs8_frag";
    case StencilSample::LeastDepth:
        return s8 ? nullptr : "raw.stencil_bits.stencil_bits_ms_d32fs8_min_frag";
    case StencilSample::GreatestDepth:
        return s8 ? nullptr : "raw.stencil_bits.stencil_bits_ms_d32fs8_max_frag";
    }
    return nullptr;
}

// The texture header word 0 NVK samples a zeta format's stencil aspect through: NIL's row of util_format_stencil_only of
// the image's format (nvk_image_view.c)
struct StencilView {
    uint32_t metal, word0;
};

constexpr StencilView kStencilViews[] = {
    {253, 0x6014921d},  // Stencil8: S8_UINT
    {260, 0x601c9205},  // Depth32Float_Stencil8: X32_S8X24_UINT
};

// 0 for a format without stencil
inline uint32_t stencilViewWord0(uint32_t format)
{
    for (const StencilView &v : kStencilViews)
        if (v.metal == format)
            return v.word0;
    return 0;
}

// Whether turn copies move every aspect of zeta format `format`: depth through its turn depth state, stencil by bits
inline bool turnMovesZeta(uint32_t format)
{
    return zetaFormatIndex(format) < kZetaFormats && (!zetaHasDepth(format) || turnDepthState(format)) &&
           (!zetaHasStencil(format) || stencilBitsState(format, 1, StencilSample::Zero));
}

}  // namespace raw
