// SPDX-License-Identifier: MIT
// The raw path's depth/stencil: Metal's Depth32Float_Stencil8 (260) as the zeta target NVK makes of m2v's
// VK_FORMAT_D32_SFLOAT_S8_UINT image, and MTLDepthStencilState as the registers NVK leaves for m2v's Vulkan form of it.
//
//  - Layout: NIL's FermiZS tiling of a one-level 2D image of 8-byte texels. Its GOBs are 64 B x 8 rows like a colour image's,
//    so the block height, row stride and size are fx::mipChainLayout's at one level; the GOB interior is the hardware's, set
//    by the PTE kind (nil nvc0_choose_pte_kind, uncompressed: the darwin nvkmd has no compression). The CPU never reads it.
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

constexpr uint32_t kZetaFormat = 260;    // MTLPixelFormatDepth32Float_Stencil8
constexpr uint32_t kZetaPteKind = 0xc3;  // NV_MMU_PTE_KIND_ZF32_X24S8 (gm107 dev_mmu.h)

// width x height of Depth32Float_Stencil8: its layout in t (block-linear, pitch the row stride, blockHeight, layerBytes NIL's
// array stride); `bytes` and `align` what its memory takes. False: no such image (a size of 0 or past 16384).
bool zetaShape(uint32_t width, uint32_t height, Texture &t, uint64_t &bytes, uint64_t &align);

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

// What the GPU holds at a draw of a pass with (`zeta`) or without a depth/stencil attachment under state `d` and stencil
// references `refFront`/`refBack` (setStencilFrontReferenceValue:backReferenceValue:)
DepthStencilRegs depthStencilRegs(const DepthStencilDesc &d, uint32_t refFront, uint32_t refBack, bool zeta);
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
