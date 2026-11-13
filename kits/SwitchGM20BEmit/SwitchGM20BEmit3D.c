// SPDX-License-Identifier: MIT
// The targets' and clears' words follow Mesa NVK's nvk_CmdBeginRendering and nvk_CmdClearAttachments, the blend NVK's
// nvk_flush_cb_state (nvk_cmd_draw.c, MIT), for one target at a time.
#include "SwitchGM20BEmit3D.h"

#include "nv_push_clb197.h"

// The float methods take the value's bits
static uint32_t fui(float f)
{
    union {
        float f;
        uint32_t u;
    } bits = {f};

    return bits.u;
}

void switch_gm20b_color_target(struct nv_push *p, uint32_t index, const struct switch_gm20b_target *t, bool compressed)
{
    P_MTHD(p, NVB197, SET_COLOR_TARGET_A(index));
    P_NVB197_SET_COLOR_TARGET_A(p, index, t->address >> 32);
    P_NVB197_SET_COLOR_TARGET_B(p, index, t->address & 0xffffffff);
    P_NVB197_SET_COLOR_TARGET_WIDTH(p, index, t->width);
    P_NVB197_SET_COLOR_TARGET_HEIGHT(p, index, t->height);
    P_NVB197_SET_COLOR_TARGET_FORMAT(p, index, t->format);
    P_NVB197_SET_COLOR_TARGET_MEMORY(
        p, index,
        {
            .block_width = BLOCK_WIDTH_ONE_GOB,
            .block_height = t->pitch ? 0 : t->block_height_log2,
            .block_depth = t->pitch ? BLOCK_DEPTH_ONE_GOB : t->block_depth_log2,
            .layout = t->pitch ? LAYOUT_PITCH : LAYOUT_BLOCKLINEAR,
            .third_dimension_control = t->depth && !t->pitch
                                           ? THIRD_DIMENSION_CONTROL_THIRD_DIMENSION_DEFINES_DEPTH_SIZE
                                           : THIRD_DIMENSION_CONTROL_THIRD_DIMENSION_DEFINES_ARRAY_SIZE,
        });
    P_NVB197_SET_COLOR_TARGET_THIRD_DIMENSION(p, index, t->layers);
    P_NVB197_SET_COLOR_TARGET_ARRAY_PITCH(p, index, t->array_pitch);
    P_NVB197_SET_COLOR_TARGET_LAYER(p, index, t->layer);
    P_IMMD(p, NVB197, SET_COLOR_COMPRESSION(index), compressed);
}

void switch_gm20b_render_area(struct nv_push *p, uint32_t count, uint32_t width, uint32_t height)
{
    P_IMMD(p, NVB197, SET_CT_SELECT,
           {
               .target_count = count,
               .target0 = 0,
               .target1 = 1,
               .target2 = 2,
               .target3 = 3,
               .target4 = 4,
               .target5 = 5,
               .target6 = 6,
               .target7 = 7,
           });
    P_MTHD(p, NVB197, SET_SURFACE_CLIP_HORIZONTAL);
    P_NVB197_SET_SURFACE_CLIP_HORIZONTAL(p, {.x = 0, .width = width});
    P_NVB197_SET_SURFACE_CLIP_VERTICAL(p, {.y = 0, .height = height});
}

void switch_gm20b_viewport(struct nv_push *p, uint32_t index, const struct switch_gm20b_viewport_state *v)
{
    P_MTHD(p, NVB197, SET_VIEWPORT_SCALE_X(index));
    P_NVB197_SET_VIEWPORT_SCALE_X(p, index, fui(v->scale[0]));
    P_NVB197_SET_VIEWPORT_SCALE_Y(p, index, fui(v->scale[1]));
    P_NVB197_SET_VIEWPORT_SCALE_Z(p, index, fui(v->scale[2]));
    P_NVB197_SET_VIEWPORT_OFFSET_X(p, index, fui(v->offset[0]));
    P_NVB197_SET_VIEWPORT_OFFSET_Y(p, index, fui(v->offset[1]));
    P_NVB197_SET_VIEWPORT_OFFSET_Z(p, index, fui(v->offset[2]));
    P_MTHD(p, NVB197, SET_VIEWPORT_CLIP_HORIZONTAL(index));
    P_NVB197_SET_VIEWPORT_CLIP_HORIZONTAL(p, index, {.x0 = v->clip_x, .width = v->clip_width});
    P_NVB197_SET_VIEWPORT_CLIP_VERTICAL(p, index, {.y0 = v->clip_y, .height = v->clip_height});
}

void switch_gm20b_viewport_swizzle(struct nv_push *p, uint32_t index, uint32_t swizzle)
{
    P_IMMD(p, NVB197, SET_VIEWPORT_COORDINATE_SWIZZLE(index),
           {
               .x = swizzle & 7,
               .y = (swizzle >> 4) & 7,
               .z = (swizzle >> 8) & 7,
               .w = (swizzle >> 12) & 7,
           });
}

void switch_gm20b_scissor(struct nv_push *p, uint32_t index, uint16_t xmin, uint16_t xmax, uint16_t ymin, uint16_t ymax)
{
    P_MTHD(p, NVB197, SET_SCISSOR_ENABLE(index));
    P_NVB197_SET_SCISSOR_ENABLE(p, index, V_TRUE);
    P_NVB197_SET_SCISSOR_HORIZONTAL(p, index, {.xmin = xmin, .xmax = xmax});
    P_NVB197_SET_SCISSOR_VERTICAL(p, index, {.ymin = ymin, .ymax = ymax});
}

void switch_gm20b_blend_constant(struct nv_push *p, const float rgba[4])
{
    P_MTHD(p, NVB197, SET_BLEND_CONST_RED);
    P_NVB197_SET_BLEND_CONST_RED(p, fui(rgba[0]));
    P_NVB197_SET_BLEND_CONST_GREEN(p, fui(rgba[1]));
    P_NVB197_SET_BLEND_CONST_BLUE(p, fui(rgba[2]));
    P_NVB197_SET_BLEND_CONST_ALPHA(p, fui(rgba[3]));
}

void switch_gm20b_clear(struct nv_push *p, const struct switch_gm20b_clear_state *c)
{
    P_MTHD(p, NVB197, SET_COLOR_CLEAR_VALUE(0));
    for (uint32_t i = 0; i < 4; i++)
        P_NVB197_SET_COLOR_CLEAR_VALUE(p, i, c->value[i]);
    P_MTHD(p, NVB197, SET_CLEAR_RECT_HORIZONTAL);
    P_NVB197_SET_CLEAR_RECT_HORIZONTAL(p, {.xmin = c->xmin, .xmax = c->xmax});
    P_NVB197_SET_CLEAR_RECT_VERTICAL(p, {.ymin = c->ymin, .ymax = c->ymax});
    P_IMMD(p, NVB197, SET_CLEAR_SURFACE_CONTROL,
           {
               .respect_stencil_mask = RESPECT_STENCIL_MASK_FALSE,
               .use_clear_rect = USE_CLEAR_RECT_TRUE,
               .use_scissor0 = USE_SCISSOR0_FALSE,
               .use_viewport_clip0 = USE_VIEWPORT_CLIP0_FALSE,
           });
    P_IMMD(p, NVB197, SET_RENDER_ENABLE_OVERRIDE, MODE_ALWAYS_RENDER);
#define CLEAR_SURFACE_FIELDS                                                                                       \
    {                                                                                                              \
        .z_enable = c->depth, .stencil_enable = c->stencil, .r_enable = R_ENABLE_TRUE, .g_enable = G_ENABLE_TRUE,  \
        .b_enable = B_ENABLE_TRUE, .a_enable = A_ENABLE_TRUE, .mrt_select = c->target, .rt_array_index = c->layer, \
    }
    if (c->layers > 1) {
        uint32_t payload;
        V_NVB197_CLEAR_SURFACE(payload, CLEAR_SURFACE_FIELDS);
        P_1INC(p, NVB197, CALL_MME_MACRO(c->layer_macro));
        P_INLINE_DATA(p, payload);
        P_INLINE_DATA(p, c->layers);
    } else {
        P_IMMD(p, NVB197, CLEAR_SURFACE, CLEAR_SURFACE_FIELDS);
    }
#undef CLEAR_SURFACE_FIELDS
    P_IMMD(p, NVB197, SET_RENDER_ENABLE_OVERRIDE, MODE_USE_RENDER_ENABLE);
}

void switch_gm20b_blend(struct nv_push *p, uint32_t index, const struct switch_gm20b_blend_state *b)
{
    P_IMMD(p, NVB197, SET_BLEND(index), b != NULL);
    if (b == NULL)
        return;
    P_MTHD(p, NVB197, SET_BLEND_PER_TARGET_SEPARATE_FOR_ALPHA(index));
    P_NVB197_SET_BLEND_PER_TARGET_SEPARATE_FOR_ALPHA(p, index, ENABLE_TRUE);
    P_NVB197_SET_BLEND_PER_TARGET_COLOR_OP(p, index, b->color_op);
    P_NVB197_SET_BLEND_PER_TARGET_COLOR_SOURCE_COEFF(p, index, b->color_source);
    P_NVB197_SET_BLEND_PER_TARGET_COLOR_DEST_COEFF(p, index, b->color_dest);
    P_NVB197_SET_BLEND_PER_TARGET_ALPHA_OP(p, index, b->alpha_op);
    P_NVB197_SET_BLEND_PER_TARGET_ALPHA_SOURCE_COEFF(p, index, b->alpha_source);
    P_NVB197_SET_BLEND_PER_TARGET_ALPHA_DEST_COEFF(p, index, b->alpha_dest);
}

void switch_gm20b_ct_write(struct nv_push *p, uint32_t index, uint32_t rgba)
{
    P_IMMD(p, NVB197, SET_CT_WRITE(index),
           {
               .r_enable = (rgba >> 0) & 1,
               .g_enable = (rgba >> 1) & 1,
               .b_enable = (rgba >> 2) & 1,
               .a_enable = (rgba >> 3) & 1,
           });
}
