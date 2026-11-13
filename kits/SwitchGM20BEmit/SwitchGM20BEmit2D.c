// SPDX-License-Identifier: MIT
#include "SwitchGM20BEmit2D.h"

#include "nv_push_cl902d.h"

uint32_t switch_gm20b_2d_fill_format(uint32_t format)
{
    switch (format) {
    case NV902D_SET_DST_FORMAT_V_A8RL8GL8BL8:
        return NV902D_SET_DST_FORMAT_V_A8R8G8B8;
    case NV902D_SET_DST_FORMAT_V_A8BL8GL8RL8:
        return NV902D_SET_DST_FORMAT_V_A8B8G8R8;
    case NV902D_SET_DST_FORMAT_V_X8RL8GL8BL8:
        return NV902D_SET_DST_FORMAT_V_X8R8G8B8;
    case NV902D_SET_DST_FORMAT_V_X8BL8GL8RL8:
        return NV902D_SET_DST_FORMAT_V_X8B8G8R8;
    default:
        return format;
    }
}

static void destination(struct nv_push *p, const struct switch_gm20b_2d_surface *s, uint32_t format)
{
    P_MTHD(p, NV902D, SET_DST_FORMAT);
    P_INLINE_DATA(p, format);
    if (s->pitch) {
        P_INLINE_DATA(p, NV902D_SET_DST_MEMORY_LAYOUT_V_PITCH);
        P_MTHD(p, NV902D, SET_DST_PITCH);
        P_INLINE_DATA(p, s->pitch_bytes);
    } else {
        P_INLINE_DATA(p, NV902D_SET_DST_MEMORY_LAYOUT_V_BLOCKLINEAR);
        P_INLINE_DATA(p, (uint32_t)s->block_height_log2 << 4);
        P_INLINE_DATA(p, 1);
        P_IMMD(p, NV902D, SET_DST_LAYER, 0);
        P_MTHD(p, NV902D, SET_DST_WIDTH);
    }
    P_INLINE_DATA(p, s->width);
    P_INLINE_DATA(p, s->height);
    P_INLINE_DATA(p, (uint32_t)(s->address >> 32));
    P_INLINE_DATA(p, (uint32_t)s->address);
}

static void source(struct nv_push *p, const struct switch_gm20b_2d_surface *s)
{
    P_MTHD(p, NV902D, SET_SRC_FORMAT);
    P_INLINE_DATA(p, s->format);
    if (s->pitch) {
        P_INLINE_DATA(p, NV902D_SET_SRC_MEMORY_LAYOUT_V_PITCH);
        P_MTHD(p, NV902D, SET_SRC_PITCH);
        P_INLINE_DATA(p, s->pitch_bytes);
    } else {
        P_INLINE_DATA(p, NV902D_SET_SRC_MEMORY_LAYOUT_V_BLOCKLINEAR);
        P_INLINE_DATA(p, (uint32_t)s->block_height_log2 << 4);
        P_INLINE_DATA(p, 1);
        P_MTHD(p, NV902D, SET_SRC_WIDTH);
    }
    P_INLINE_DATA(p, s->width);
    P_INLINE_DATA(p, s->height);
    P_INLINE_DATA(p, (uint32_t)(s->address >> 32));
    P_INLINE_DATA(p, (uint32_t)s->address);
}

// The pattern a ROP3 code may read held at all ones, as the DDX holds it outside planemask copies (NVC0EXASetPattern)
static void rop_on(struct nv_push *p, uint8_t rop)
{
    P_IMMD(p, NV902D, SET_OPERATION, V_ROP);
    P_IMMD(p, NV902D, SET_ROP, rop);
    P_MTHD(p, NV902D, SET_MONOCHROME_PATTERN_COLOR_FORMAT);
    P_INLINE_DATA(p, NV902D_SET_MONOCHROME_PATTERN_COLOR_FORMAT_V_A8R8G8B8);
    P_INLINE_DATA(p, NV902D_SET_MONOCHROME_PATTERN_FORMAT_V_LE_M1);
    P_INLINE_DATA(p, ~0u);
    P_INLINE_DATA(p, ~0u);
    P_INLINE_DATA(p, ~0u);
    P_INLINE_DATA(p, ~0u);
}

void switch_gm20b_2d_scale(struct nv_push *p, const struct switch_gm20b_2d_surface *src,
                           const struct switch_gm20b_2d_surface *dst, const struct switch_gm20b_2d_scale_op *s)
{
    P_IMMD(p, NV902D, SET_DST_COLOR_RENDER_TO_ZETA_SURFACE, dst->zeta);
    P_IMMD(p, NV902D, SET_COMPRESSION, dst->compressed);
    destination(p, dst, dst->format);
    source(p, src);
    if (s->rop)
        rop_on(p, s->rop);
    P_IMMD(p, NV902D, TWOD_INVALIDATE_TEXTURE_DATA_CACHE, V_L1_ONLY);
    P_IMMD(
        p, NV902D, SET_PIXELS_FROM_MEMORY_SAMPLE_MODE,
        {.origin = s->corner ? ORIGIN_CORNER : ORIGIN_CENTER, .filter = s->bilinear ? FILTER_BILINEAR : FILTER_POINT});
    P_MTHD(p, NV902D, SET_PIXELS_FROM_MEMORY_DST_X0);
    P_INLINE_DATA(p, s->dst_x);
    P_INLINE_DATA(p, s->dst_y);
    P_INLINE_DATA(p, s->dst_w);
    P_INLINE_DATA(p, s->dst_h);
    P_MTHD(p, NV902D, SET_PIXELS_FROM_MEMORY_DU_DX_FRAC);
    P_INLINE_DATA(p, (uint32_t)s->du);
    P_INLINE_DATA(p, (uint32_t)(s->du >> 32));
    P_INLINE_DATA(p, (uint32_t)s->dv);
    P_INLINE_DATA(p, (uint32_t)(s->dv >> 32));
    P_MTHD(p, NV902D, SET_PIXELS_FROM_MEMORY_SRC_X0_FRAC);
    P_INLINE_DATA(p, (uint32_t)s->src_x);
    P_INLINE_DATA(p, (uint32_t)(s->src_x >> 32));
    P_INLINE_DATA(p, (uint32_t)s->src_y);
    P_INLINE_DATA(p, (uint32_t)(s->src_y >> 32));
    if (s->rop)
        P_IMMD(p, NV902D, SET_OPERATION, V_SRCCOPY);
}

static void solid(struct nv_push *p, const struct switch_gm20b_2d_surface *dst, uint8_t rop, uint32_t color,
                  uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    const uint32_t format = switch_gm20b_2d_fill_format(dst->format);
    P_IMMD(p, NV902D, SET_DST_COLOR_RENDER_TO_ZETA_SURFACE, dst->zeta);
    P_IMMD(p, NV902D, SET_COMPRESSION, dst->compressed);
    destination(p, dst, format);
    if (rop)
        rop_on(p, rop);
    else
        P_IMMD(p, NV902D, SET_OPERATION, V_SRCCOPY);
    P_IMMD(p, NV902D, SET_COLOR_KEY_ENABLE, V_FALSE);
    P_IMMD(p, NV902D, SET_RENDER_ENABLE_C, MODE_TRUE);
    P_MTHD(p, NV902D, SET_CLIP_X0);
    P_INLINE_DATA(p, 0);
    P_INLINE_DATA(p, 0);
    P_INLINE_DATA(p, dst->width);
    P_INLINE_DATA(p, dst->height);
    P_IMMD(p, NV902D, SET_CLIP_ENABLE, V_TRUE);
    P_MTHD(p, NV902D, RENDER_SOLID_PRIM_MODE);
    P_INLINE_DATA(p, NV902D_RENDER_SOLID_PRIM_MODE_V_RECTS);
    P_INLINE_DATA(p, format);
    P_INLINE_DATA(p, color);
    P_MTHD(p, NV902D, RENDER_SOLID_PRIM_POINT_SET_X(0));
    P_INLINE_DATA(p, x0);
    P_INLINE_DATA(p, y0);
    P_INLINE_DATA(p, x1);
    P_INLINE_DATA(p, y1);
    P_IMMD(p, NV902D, SET_CLIP_ENABLE, V_FALSE);
    if (rop)
        P_IMMD(p, NV902D, SET_OPERATION, V_SRCCOPY);
}

void switch_gm20b_2d_fill(struct nv_push *p, const struct switch_gm20b_2d_surface *dst, uint32_t color, uint32_t x0,
                          uint32_t y0, uint32_t x1, uint32_t y1)
{
    solid(p, dst, 0, color, x0, y0, x1, y1);
}

void switch_gm20b_2d_invert(struct nv_push *p, const struct switch_gm20b_2d_surface *dst, uint32_t x0, uint32_t y0,
                            uint32_t x1, uint32_t y1)
{
    solid(p, dst, SWITCH_GM20B_2D_ROP_DSTINVERT, 0, x0, y0, x1, y1);
}
