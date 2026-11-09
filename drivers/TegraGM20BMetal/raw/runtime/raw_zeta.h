// SPDX-License-Identifier: MIT
// The raw path's depth/stencil: Metal's Depth16Unorm (250), Depth32Float (252), Stencil8 (253) and Depth32Float_Stencil8 (260),
// one sample or several, as the zeta target NVK makes of m2v's VK_FORMAT_D16_UNORM, _D32_SFLOAT, _S8_UINT and
// _D32_SFLOAT_S8_UINT image, and MTLDepthStencilState as the registers NVK leaves for m2v's Vulkan form of it.
//
//  - Layout: NIL's FermiZS tiling of a one-level 2D image (several samples: its sample grid, fx::msLayout). Its GOBs are 64 B x
//    8 rows like a colour image's, so the block height, row stride and size are fx::mipChainLayout's at one level; the GOB
//    interior is the hardware's, set by the PTE kind (nil nvc0_choose_pte_kind: Z16, ZF32 or ZF32_X24S8 whatever the samples;
//    one-sample Depth32Float_Stencil8 also its compressible ZF32_X24S8_2CSZV on compressed memory, raw_comp.h). The CPU never
//    reads it.
//  - Target: nvk_CmdBeginRendering's SET_ZT_* for one layer. NVK keeps no zcull here (the kext reports no zcull info), so
//    SET_ACTIVE_ZCULL_REGION stays the baseline's 0x3f.
//  - State: metal2vulkan state.rs depth_stencil_state (a depth test where the compare is not Always or depth writes, a
//    stencil test where either face is not a no-op), then nvk_flush_ds_state: the enables only with an attachment of the
//    aspect, masks cut to 8 bits by Mesa's dynamic state, references saturated at 255 (metal2vulkan render_state.rs, as
//    Metal measured).
#pragma once
#include <cstdint>
#include "raw_push.h"

namespace raw {

struct Texture;

constexpr uint32_t kZetaFormat = 260;  // MTLPixelFormatDepth32Float_Stencil8

// A depth/stencil format the raw path renders: its MTLPixelFormat, SET_ZT_FORMAT, texel bytes, PTE kind (gp100 dev_mmu.h,
// gm200_mmu_kind's on the GM20B), its aspects, and the texture header word 0 NVK samples it through (nil_formats.csv's row of
// the image's format, which NVK's depth-aspect view keeps: R the depth, or Stencil8's stencil, 0 0 1)
struct ZetaFormat {
    uint32_t metal, zt, bpp, kind;
    bool depth, stencil;
    uint32_t tic0;
};

constexpr uint32_t kZetaFormats = 4;
constexpr ZetaFormat kZetaFormatInfo[kZetaFormats] = {
    {250, 0x13, 2, 0x01, true, false, 0x7012493a},  // Depth16Unorm: Z16, UNORM
    {252, 0x0a, 4, 0x7b, true, false, 0x7017ffaf},  // Depth32Float: ZF32, FLOAT
    {253, 0x17, 1, 0x2a, false, true, 0x6014921d},  // Stencil8: S8, sampled as R8 UINT
    {260, 0x19, 8, 0xc3, true, true, 0x7017f3b0},   // Depth32Float_Stencil8: ZF32_X24S8, FLOAT depth, UINT stencil
};

// its index in kZetaFormatInfo, kZetaFormats for none
inline uint32_t zetaFormatIndex(uint32_t metal)
{
    uint32_t i = 0;
    while (i < kZetaFormats && kZetaFormatInfo[i].metal != metal)
        i++;
    return i;
}

inline bool zetaHasDepth(uint32_t metal)
{
    const uint32_t i = zetaFormatIndex(metal);
    return i < kZetaFormats && kZetaFormatInfo[i].depth;
}

inline bool zetaHasStencil(uint32_t metal)
{
    const uint32_t i = zetaFormatIndex(metal);
    return i < kZetaFormats && kZetaFormatInfo[i].stencil;
}

// width x height of zeta `format` at `samples`: its layout in t (block-linear, pitch the row stride, blockHeight, layerBytes NIL's
// array stride); `bytes` and `align` what its memory takes. False: no such image (another format, a size of 0 or past 16384).
bool zetaShape(uint32_t width, uint32_t height, uint32_t format, uint32_t samples, Texture &t, uint64_t &bytes,
               uint64_t &align);

// SET_ZT_A .. SET_STENCIL_COMPRESSION and SET_ZT_SPARSE for zeta `z`, or SET_ZT_SELECT 0 for none
constexpr uint32_t kZetaTargetWords = 24;
void zetaTargetWords(Push &p, const Texture *z);

// MTLStencilDescriptor and MTLDepthStencilDescriptor, enums as Metal numbers them (MTLCompareFunction, MTLStencilOperation)
struct StencilFace {
    uint8_t compare = 7;                        // Always
    uint8_t fail = 0, depthFail = 0, pass = 0;  // Keep
    uint32_t readMask = ~0u, writeMask = ~0u;
};

struct DepthStencilDesc {
    uint8_t depthCompare = 7;  // Always
    bool depthWrite = false;
    StencilFace front, back;

    // whether a draw under it tests or writes depth or stencil (a pass without the attachment cannot take it as Metal does)
    bool active() const;
};

// The depth/stencil registers' values (kDepthStencilMethods order)
constexpr uint32_t kDepthStencilRegs = 18;

struct DepthStencilRegs {
    uint32_t v[kDepthStencilRegs] = {};

    bool operator==(const DepthStencilRegs &o) const;
};

// What the GPU holds at a draw of a pass with a depth/stencil attachment of the aspects `depth` and `stencil`, or without one,
// under state `d` and stencil references `refFront`/`refBack` (setStencilFrontReferenceValue:backReferenceValue:)
DepthStencilRegs depthStencilRegs(const DepthStencilDesc &d, uint32_t refFront, uint32_t refBack, bool depth,
                                  bool stencil);
// The methods that take `held` (what the GPU holds) to `want`, then held = want
constexpr uint32_t kDepthStencilWords = 2 * kDepthStencilRegs;
void emitDepthStencil(Push &p, const DepthStencilRegs &want, DepthStencilRegs &held);

// A depth/stencil clear before a pass's draws (nvk_CmdClearAttachments): its values, then the CLEAR_SURFACE bits of its aspects
constexpr uint32_t kZetaClearWords = 4;
void zetaClearValues(Push &p, bool depth, float clearDepth, bool stencil, uint32_t clearStencil);

inline uint32_t zetaClearBits(bool depth, bool stencil)
{
    return (depth ? 1u : 0u) | (stencil ? 2u : 0u);  // CLEAR_SURFACE Z_ENABLE, STENCIL_ENABLE
}

}  // namespace raw
