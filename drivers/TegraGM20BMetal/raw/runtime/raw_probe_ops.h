// SPDX-License-Identifier: MIT
// hw_test_app's engine probes (app27/X1TwodProbes.swift), debug builds only: FERMI_TWOD_A work between the probe's own
// textures where Metal has no call for it (a scaled copy, a solid fill) or where the raw path draws today (a multisample
// resolve, read as the sample grid's image at 2:1), emitted inside a blit encoder (BlitEncoder::probe). No wait of their
// own: the blit encoder's opening and the next pass's prologue order them, as for every other blit. Surfaces as
// raw/fx/fx_mip.cpp writes them (nvc0_2d_texture_set); the solid fill as nouveau's 2D rectangle fills write it (DRAW_SHAPE,
// its colour format and colour, then the two corners); the reference fill first sets the state nouveau's DDX gives the
// object (nvc0_accel.c, nvc0_exa.c: SRCCOPY, colour key off, render enabled, the clip at the surface) and turns the clip
// off after, as raw/fx/fx_mip.cpp expects it.
#pragma once
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
#include <cstddef>
#include <cstdint>
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "raw_push.h"

namespace raw {

struct ProbeSurface {
    uint64_t va = 0;
    uint32_t fmt2d = 0, pitch = 0, width = 0, height = 0;
    bool blockLinear = false;
    uint8_t blockHeight = 0;  // log2 GOBs
};

enum class ProbeOpKind : uint8_t {
    Scale,
    Fill,
    Method  // one 3D-class method and its value (gpu.recover.kill_one's illegal one)
};

struct ProbeOp {
    ProbeOpKind kind = ProbeOpKind::Scale;
    ProbeSurface src, dst;
    uint32_t dx = 0, dy = 0, dw = 0, dh = 0;  // the destination rectangle [dx, dx + dw) x [dy, dy + dh)
    // Scale: the source step per destination texel and the source origin, 32.32; SAMPLE_MODE's filter and origin
    uint64_t du = 1ull << 32, dv = 1ull << 32, sx = 0, sy = 0;
    bool bilinear = false, corner = false;
    // Fill: SET_RENDER_SOLID_PRIM_COLOR_FORMAT, and its colour: one word, or four floats' bits for RF32_GF32_BF32_AF32
    uint32_t colorFormat = 0, color[4] = {};
    bool referenceFill = false;
    // Between the waits the production 2D path puts around 2D work (raw/fx/fx_mip.cpp begin and end): the tiled
    // cache drained and the 3D engine idle before; the 2D engine, then the 3D engine idle and the texture cache
    // invalidated after. The first boots' fills ran without them, as the last GR work after clear-only passes
    bool bracketed = true;
    // Fill and Scale: the 2D engine's clip rectangle [x, x + w) x [y, y + h) around the operation, cleared after it; w 0:
    // none (the reference fill's surface clip stays)
    uint32_t clip[4] = {};
    uint32_t method = 0, value = 0;
    // Fill and Scale: 0 SRCCOPY; else a ROP3 code on the destination's codes (SET_OPERATION ROP, xf86-video-nouveau's
    // NVC0EXASetROP form, the pattern held at all ones), SRCCOPY set again after
    uint8_t rop = 0;
};

constexpr uint32_t kProbeBracketWords = 2 + 2 + 2 + 2 + 2;

inline void probeOpAfter(Push &p)
{
    p.m(NV902D_WAIT_FOR_IDLE, 0, SUBC_2D);
    p.m(NVB197_WAIT_FOR_IDLE, 0);
    p.m(NVB197_INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
}

constexpr uint32_t kTwodColorF32x4 = 0xc0;

namespace probe_ops_detail {

constexpr uint32_t kSurfaceWords = 1 + 4 + 2 + 1 + 4;
constexpr uint32_t kReferenceFillWords = 2 + 2 + 2 + 5 + 2 + 2;
constexpr uint32_t kClipWords = 5 + 2 + 2;
constexpr uint32_t kRopWords = 2 + 2 + 7 + 2;

inline void ropOn(Push &p, const ProbeOp &op)
{
    if (!op.rop)
        return;
    p.m(NV902D_SET_OPERATION, NV902D_SET_OPERATION_V_ROP, SUBC_2D);
    p.m(NV902D_SET_ROP, op.rop, SUBC_2D);
    p.inc(NV902D_SET_MONOCHROME_PATTERN_COLOR_FORMAT,
          {NV902D_SET_MONOCHROME_PATTERN_COLOR_FORMAT_V_A8R8G8B8, NV902D_SET_MONOCHROME_PATTERN_FORMAT_V_LE_M1, ~0u,
           ~0u, ~0u, ~0u},
          SUBC_2D);
}

inline void ropOff(Push &p, const ProbeOp &op)
{
    if (op.rop)
        p.m(NV902D_SET_OPERATION, NV902D_SET_OPERATION_V_SRCCOPY, SUBC_2D);
}

inline void clipOn(Push &p, const ProbeOp &op)
{
    if (!op.clip[2])
        return;
    p.inc(NV902D_SET_CLIP_X0, {op.clip[0], op.clip[1], op.clip[2], op.clip[3]}, SUBC_2D);
    p.m(NV902D_SET_CLIP_ENABLE, NV902D_SET_CLIP_ENABLE_V_TRUE, SUBC_2D);
}

inline void surface(Push &p, uint32_t base, const ProbeSurface &s)
{
    if (s.blockLinear) {
        p.inc(base, {s.fmt2d, NV902D_SET_DST_MEMORY_LAYOUT_V_BLOCKLINEAR, uint32_t(s.blockHeight) << 4, 1}, SUBC_2D);
        if (base == NV902D_SET_DST_FORMAT)
            p.m(base + 0x10, 0, SUBC_2D);  // DST_LAYER
        p.inc(base + 0x18, {s.width, s.height, uint32_t(s.va >> 32), uint32_t(s.va)}, SUBC_2D);
    } else {
        p.inc(base, {s.fmt2d, NV902D_SET_DST_MEMORY_LAYOUT_V_PITCH}, SUBC_2D);
        p.inc(base + 0x14, {s.pitch, s.width, s.height, uint32_t(s.va >> 32), uint32_t(s.va)}, SUBC_2D);
    }
}

}  // namespace probe_ops_detail

inline uint32_t probeTwodFormat(uint32_t mtl)
{
    switch (mtl) {
    case 10:
        return NV902D_SET_DST_FORMAT_V_Y8;  // R8Unorm, as fx::twodFormat
    case 20:
        return NV902D_SET_DST_FORMAT_V_Y16;  // R16Unorm
    case 25:
        return NV902D_SET_DST_FORMAT_V_RF16;  // R16Float
    case 30:
        return NV902D_SET_DST_FORMAT_V_G8R8;  // RG8Unorm
    case 60:
        return NV902D_SET_DST_FORMAT_V_R16_G16;  // RG16Unorm
    case 65:
        return NV902D_SET_DST_FORMAT_V_RF16_GF16;  // RG16Float
    case 70:
        return NV902D_SET_DST_FORMAT_V_A8B8G8R8;  // RGBA8Unorm
    case 71:
        return NV902D_SET_DST_FORMAT_V_A8BL8GL8RL8;  // RGBA8Unorm_sRGB
    case 80:
        return NV902D_SET_DST_FORMAT_V_A8R8G8B8;  // BGRA8Unorm
    case 81:
        return NV902D_SET_DST_FORMAT_V_A8RL8GL8BL8;  // BGRA8Unorm_sRGB
    case 90:
        return NV902D_SET_DST_FORMAT_V_A2B10G10R10;  // RGB10A2Unorm
    case 92:
        return NV902D_SET_DST_FORMAT_V_BF10GF11RF11;  // RG11B10Float
    case 110:
        return NV902D_SET_DST_FORMAT_V_R16_G16_B16_A16;  // RGBA16Unorm
    case 115:
        return NV902D_SET_DST_FORMAT_V_RF16_GF16_BF16_AF16;  // RGBA16Float
    case 125:
        return NV902D_SET_DST_FORMAT_V_RF32_GF32_BF32_AF32;  // RGBA32Float
    default:
        return 0;
    }
}

// A solid fill in a colour format other than its destination's own (0xC0 four floats into A8R8G8B8, 12 Nov) passed its
// waits, then GR could not save the channel's context and the kernel panicked; sRGB destinations raise class error 0x4a
inline bool probeSolidFill(uint32_t fmt2d, uint32_t colorFormat)
{
    switch (fmt2d) {
    case NV902D_SET_DST_FORMAT_V_A8R8G8B8:
    case NV902D_SET_DST_FORMAT_V_A8B8G8R8:
    case NV902D_SET_DST_FORMAT_V_A2B10G10R10:
    case NV902D_SET_DST_FORMAT_V_Y8:
    case NV902D_SET_DST_FORMAT_V_Y16:
    case NV902D_SET_DST_FORMAT_V_RF16_GF16_BF16_AF16:
    case NV902D_SET_DST_FORMAT_V_RF32_GF32_BF32_AF32:
        return colorFormat == fmt2d;
    default:
        return false;
    }
}

inline bool probeResolveOp(const ProbeSurface &ms, uint32_t sxLog2, uint32_t syLog2, const ProbeSurface &into,
                           ProbeOp &op)
{
    if (sxLog2 > 1 || syLog2 > 1 || !into.width || !into.height || ms.width != into.width << sxLog2 ||
        ms.height != into.height << syLog2)
        return false;
    op = {};
    op.kind = ProbeOpKind::Scale;
    op.src = ms;
    op.dst = into;
    op.dw = into.width;
    op.dh = into.height;
    op.du = (1ull << 32) << sxLog2;
    op.dv = (1ull << 32) << syLog2;
    op.sx = op.du / 2;
    op.sy = op.dv / 2;
    op.bilinear = sxLog2 || syLog2;
    op.corner = true;
    return true;
}

inline size_t probeOpWords(const ProbeOp &op)
{
    if (op.kind == ProbeOpKind::Method)
        return 2;
    if (op.kind == ProbeOpKind::Fill)
        return 2 + probe_ops_detail::kSurfaceWords + 2 + 5 + 4 + 5 + probe_ops_detail::kReferenceFillWords +
               probe_ops_detail::kClipWords + probe_ops_detail::kRopWords;
    return 2 + 2 * probe_ops_detail::kSurfaceWords + 2 + 2 + 3 * 5 + probe_ops_detail::kClipWords +
           probe_ops_detail::kRopWords;
}

inline void emitProbeOp(Push &p, const ProbeOp &op)
{
    if (op.kind == ProbeOpKind::Method) {
        p.m(op.method, op.value, SUBC_3D);
        return;
    }
    p.m(NV902D_SET_DST_COLOR_RENDER_TO_ZETA_SURFACE, 0, SUBC_2D);
    probe_ops_detail::surface(p, NV902D_SET_DST_FORMAT, op.dst);
    if (op.kind == ProbeOpKind::Fill) {
        if (op.referenceFill) {
            p.m(NV902D_SET_OPERATION, NV902D_SET_OPERATION_V_SRCCOPY, SUBC_2D);
            p.m(NV902D_SET_COLOR_KEY_ENABLE, 0, SUBC_2D);
            p.m(NV902D_SET_RENDER_ENABLE_C, NV902D_SET_RENDER_ENABLE_C_MODE_TRUE, SUBC_2D);
            p.inc(NV902D_SET_CLIP_X0, {0, 0, op.dst.width, op.dst.height}, SUBC_2D);
            p.m(NV902D_SET_CLIP_ENABLE, NV902D_SET_CLIP_ENABLE_V_TRUE, SUBC_2D);
        }
        probe_ops_detail::clipOn(p, op);
        probe_ops_detail::ropOn(p, op);
        if (op.colorFormat == kTwodColorF32x4) {
            // SET_RENDER_SOLID_PRIM_COLOR (0x588) lands in COLOR0: written after the four floats it stored red 0 on chip
            p.inc(NV902D_RENDER_SOLID_PRIM_MODE, {NV902D_RENDER_SOLID_PRIM_MODE_V_RECTS, op.colorFormat}, SUBC_2D);
            p.inc(NV902D_SET_RENDER_SOLID_PRIM_COLOR0, {op.color[0], op.color[1], op.color[2], op.color[3]}, SUBC_2D);
        } else {
            p.inc(NV902D_RENDER_SOLID_PRIM_MODE, {NV902D_RENDER_SOLID_PRIM_MODE_V_RECTS, op.colorFormat, op.color[0]},
                  SUBC_2D);
        }
        p.inc(NV902D_RENDER_SOLID_PRIM_POINT_SET_X(0), {op.dx, op.dy, op.dx + op.dw, op.dy + op.dh}, SUBC_2D);
        if (op.referenceFill || op.clip[2])
            p.m(NV902D_SET_CLIP_ENABLE, NV902D_SET_CLIP_ENABLE_V_FALSE, SUBC_2D);
        probe_ops_detail::ropOff(p, op);
        return;
    }
    probe_ops_detail::surface(p, NV902D_SET_SRC_FORMAT, op.src);
    p.m(NV902D_TWOD_INVALIDATE_TEXTURE_DATA_CACHE, NVDEF(NV902D, TWOD_INVALIDATE_TEXTURE_DATA_CACHE, V, L1_ONLY),
        SUBC_2D);
    const uint32_t mode = (op.corner ? NVDEF(NV902D, SET_PIXELS_FROM_MEMORY_SAMPLE_MODE, ORIGIN, CORNER)
                                     : NVDEF(NV902D, SET_PIXELS_FROM_MEMORY_SAMPLE_MODE, ORIGIN, CENTER)) |
                          (op.bilinear ? NVDEF(NV902D, SET_PIXELS_FROM_MEMORY_SAMPLE_MODE, FILTER, BILINEAR)
                                       : NVDEF(NV902D, SET_PIXELS_FROM_MEMORY_SAMPLE_MODE, FILTER, POINT));
    p.m(NV902D_SET_PIXELS_FROM_MEMORY_SAMPLE_MODE, mode, SUBC_2D);
    p.inc(NV902D_SET_PIXELS_FROM_MEMORY_DST_X0, {op.dx, op.dy, op.dw, op.dh}, SUBC_2D);
    p.inc(NV902D_SET_PIXELS_FROM_MEMORY_DU_DX_FRAC,
          {uint32_t(op.du), uint32_t(op.du >> 32), uint32_t(op.dv), uint32_t(op.dv >> 32)}, SUBC_2D);
    probe_ops_detail::clipOn(p, op);
    probe_ops_detail::ropOn(p, op);
    p.inc(NV902D_SET_PIXELS_FROM_MEMORY_SRC_X0_FRAC,
          {uint32_t(op.sx), uint32_t(op.sx >> 32), uint32_t(op.sy), uint32_t(op.sy >> 32)}, SUBC_2D);
    if (op.clip[2])
        p.m(NV902D_SET_CLIP_ENABLE, NV902D_SET_CLIP_ENABLE_V_FALSE, SUBC_2D);
    probe_ops_detail::ropOff(p, op);
}

}  // namespace raw
#endif
