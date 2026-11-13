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
//  - Target: nvk_CmdBeginRendering's SET_ZT_* for one layer; its zcull region, or none, raw_zcull.h's.
//  - State: its register values are raw_depth_stencil_state.h's.
#pragma once
#include <algorithm>
#include <cstdint>
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"
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

// SET_ZT_A .. SET_STENCIL_COMPRESSION and SET_ZT_SPARSE for zeta `z` (its `layers` layers, layerBytes apart), or
// SET_ZT_SELECT 0 for none
constexpr uint32_t kZetaTargetWords = 24;
void zetaTargetWords(Push &p, const Texture *z, uint32_t layers = 1);

// The values of SET_ZT_A .. SET_ZT_ARRAY_PITCH and SET_ZT_SIZE_A .. C that zetaTargetWords writes for zeta `z` of `rows`
// rows (raw_samples.h sampleRows)
struct ZetaTargetValues {
    uint32_t a[5], size[3];
};

template <typename Zeta> ZetaTargetValues zetaTargetValues(const Zeta &z, uint32_t rows)
{
    return {{uint32_t(z.va >> 32), uint32_t(z.va), kZetaFormatInfo[zetaFormatIndex(z.format)].zt,
             uint32_t(z.blockHeight & 0xf) << 4,  // WIDTH ONE_GOB, HEIGHT, DEPTH ONE_GOB
             uint32_t(z.layerBytes >> 2)},
            // SIZE_A the row stride in texels (NVK: the Z/S hardware has no tile width), C one layer,
            // THIRD_DIMENSION_DEFINES_ARRAY_SIZE
            {z.pitch / z.bpp, rows, 1}};
}

// MTLStencilDescriptor and MTLDepthStencilDescriptor, enums as Metal numbers them (MTLCompareFunction, MTLStencilOperation)
struct StencilFace {
    uint8_t compare = 7;                        // Always
    uint8_t fail = 0, depthFail = 0, pass = 0;  // Keep
    uint32_t readMask = ~0u, writeMask = ~0u;
};

inline bool stencilNoop(const StencilFace &s)
{
    return s.compare == 7 && s.depthFail == 0 && s.pass == 0;
}

struct DepthStencilDesc {
    uint8_t depthCompare = 7;  // Always
    bool depthWrite = false;
    StencilFace front, back;

    // whether a draw under it tests or writes depth or stencil (a pass without the attachment cannot take it as Metal does)
    bool active() const;
};

// The depth/stencil registers' values (kDepthStencilMethods order)
constexpr uint32_t kDepthStencilRegs = 18;

inline constexpr uint32_t kDepthStencilMethods[kDepthStencilRegs] = {NVB197_SET_DEPTH_TEST,
                                                                     NVB197_SET_DEPTH_WRITE,
                                                                     NVB197_SET_DEPTH_FUNC,
                                                                     NVB197_SET_STENCIL_TEST,
                                                                     NVB197_SET_STENCIL_OP_FAIL,
                                                                     NVB197_SET_STENCIL_OP_ZFAIL,
                                                                     NVB197_SET_STENCIL_OP_ZPASS,
                                                                     NVB197_SET_STENCIL_FUNC,
                                                                     NVB197_SET_STENCIL_FUNC_REF,
                                                                     NVB197_SET_STENCIL_FUNC_MASK,
                                                                     NVB197_SET_STENCIL_MASK,
                                                                     NVB197_SET_BACK_STENCIL_OP_FAIL,
                                                                     NVB197_SET_BACK_STENCIL_OP_ZFAIL,
                                                                     NVB197_SET_BACK_STENCIL_OP_ZPASS,
                                                                     NVB197_SET_BACK_STENCIL_FUNC,
                                                                     NVB197_SET_BACK_STENCIL_FUNC_REF,
                                                                     NVB197_SET_BACK_STENCIL_MASK,
                                                                     NVB197_SET_BACK_STENCIL_FUNC_MASK};

struct DepthStencilRegs {
    uint32_t v[kDepthStencilRegs] = {};

    bool operator==(const DepthStencilRegs &o) const;
};

// The most words emitDepthStencil (raw_depth_stencil_state.h) writes
constexpr uint32_t kDepthStencilWords = 2 * kDepthStencilRegs;

// The registers of `regs` but the two stencil references as one absolute method stream, kDepthStencilMethods order: what a
// GPU-resident state segment holds whole. Every value fits an immediate method. The references are a draw's own
// (setStencilFrontReferenceValue:backReferenceValue:), set by stencilReferenceWords.
constexpr uint32_t kDepthStencilBlockWords = kDepthStencilRegs - 2;
using DepthStencilBlock = uint32_t[kDepthStencilBlockWords];

inline void depthStencilBlock(const DepthStencilRegs &regs, DepthStencilBlock &out)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < kDepthStencilRegs; i++)
        if (kDepthStencilMethods[i] != NVB197_SET_STENCIL_FUNC_REF &&
            kDepthStencilMethods[i] != NVB197_SET_BACK_STENCIL_FUNC_REF)
            out[n++] = Push::hdr(4, SUBC_3D, kDepthStencilMethods[i], regs.v[i] & 0x1fff);
}

constexpr uint32_t kStencilReferenceWords = 2;

inline void stencilReferenceWords(Push &p, uint32_t front, uint32_t back)
{
    *p.at++ = Push::hdr(4, SUBC_3D, NVB197_SET_STENCIL_FUNC_REF, std::min(front, 0xffu));
    *p.at++ = Push::hdr(4, SUBC_3D, NVB197_SET_BACK_STENCIL_FUNC_REF, std::min(back, 0xffu));
}

// A depth/stencil clear before a pass's draws (nvk_CmdClearAttachments): its values, then the CLEAR_SURFACE bits of its aspects
constexpr uint32_t kZetaClearWords = 4;
void zetaClearValues(Push &p, bool depth, float clearDepth, bool stencil, uint32_t clearStencil);

inline uint32_t zetaClearBits(bool depth, bool stencil)
{
    return (depth ? 1u : 0u) | (stencil ? 2u : 0u);  // CLEAR_SURFACE Z_ENABLE, STENCIL_ENABLE
}

}  // namespace raw
