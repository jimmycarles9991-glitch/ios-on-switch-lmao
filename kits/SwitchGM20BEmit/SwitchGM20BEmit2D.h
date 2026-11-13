// SPDX-License-Identifier: MIT
// FERMI_TWOD_A solid fills in the form xf86-video-nouveau's EXA fill writes (nvc0_exa.c NVC0EXAPrepareSolid/Solid):
// SRCCOPY, colour key off, render enable on, clip set to the destination, and the destination's own colour format with
// one packed colour word. The first boot's probe fill (RF32x4 colour format, four float words, clip off, unbracketed after
// clear-only passes) left GR busy until a context-switch timeout (x1-units UNITS.md). The caller brackets a fill as
// raw/fx/fx_mip.cpp brackets 2D work: tiled cache drained and the 3D engine idle before, the 2D then 3D engine idle and
// the texture cache invalidated after.
#pragma once

#include "../SwitchGM20B/SwitchGM20B2D.h"
#include "nv_push.h"

#ifdef __cplusplus
extern "C" {
#endif

struct switch_gm20b_2d_surface {
    uint64_t address;
    uint32_t format;  // NV902D_SET_DST_FORMAT_V_*
    bool pitch;
    uint32_t pitch_bytes;       // pitch layout
    uint8_t block_height_log2;  // block-linear layout
    uint32_t width, height;
    bool
        compressed;  // memory of a compressible kind with comptags: SET_COMPRESSION, so the engine keeps the tiles' state
    bool zeta;       // a zeta surface written as colour (SET_DST_COLOR_RENDER_TO_ZETA_SURFACE)
};

#define SWITCH_GM20B_2D_FILL_WORDS 32u

// The format a solid fill writes `format` through: an sRGB one's UNORM twin. The engine refuses a solid prim into an
// sRGB destination (ERROR2D_ILLEGAL_SRGB_DST_FORMAT_FOR_SOLID_PRIM, class error 0x4a: T-v2's gpu.x1.twod_clear), so an
// sRGB surface is filled through its UNORM view with the colour's encoded codes.
uint32_t switch_gm20b_2d_fill_format(uint32_t format);

// [x0, x1) x [y0, y1) of `dst` set to `color`, one word packed in switch_gm20b_2d_fill_format(dst->format)
void switch_gm20b_2d_fill(struct nv_push *p, const struct switch_gm20b_2d_surface *dst, uint32_t color, uint32_t x0,
                          uint32_t y0, uint32_t x1, uint32_t y1);

// A scaled copy of `src` into [dst_x, dst_x + dst_w) x [dst_y, dst_y + dst_h) of `dst`: destination texel (x, y) samples
// the source at src_x + (x + 1/2) du, src_y + (y + 1/2) dv under the centre origin, or from the corner under `corner`
// (GM20B's mip chains: corner origin, source origin at du/2, raw/fx/fx_mip.h kGm20bSampling). 32.32 fixed point. The
// engine's texture cache is invalidated (L1) before the source is read; the caller orders the source's writes before and
// the destination's readers after.
// `rop` 0 copies the filtered source (SRCCOPY); any other value is a ROP3 code the engine applies to the destination's
// codes after the filter (SET_OPERATION ROP, xf86-video-nouveau's NVC0EXASetROP form), SRCCOPY set again after it.
struct switch_gm20b_2d_scale_op {
    uint32_t dst_x, dst_y, dst_w, dst_h;
    uint64_t du, dv, src_x, src_y;
    bool bilinear, corner;
    uint8_t rop;
};

#define SWITCH_GM20B_2D_SCALE_WORDS 52u

void switch_gm20b_2d_scale(struct nv_push *p, const struct switch_gm20b_2d_surface *src,
                           const struct switch_gm20b_2d_surface *dst, const struct switch_gm20b_2d_scale_op *s);

#define SWITCH_GM20B_2D_ROP_NOTSRCCOPY 0x33u
#define SWITCH_GM20B_2D_ROP_DSTINVERT  0x55u
#define SWITCH_GM20B_2D_INVERT_WORDS   44u

// [x0, x1) x [y0, y1) of `dst` complemented in place, every channel's code c to 2^n - 1 - c: a solid rectangle under
// ROP3 DSTINVERT (xf86-video-nouveau's GXinvert fill), clip at the surface, SRCCOPY set again after it
void switch_gm20b_2d_invert(struct nv_push *p, const struct switch_gm20b_2d_surface *dst, uint32_t x0, uint32_t y0,
                            uint32_t x1, uint32_t y1);

#ifdef __cplusplus
}
#endif
