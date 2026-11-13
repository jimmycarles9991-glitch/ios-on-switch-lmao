// SPDX-License-Identifier: MIT
#include "SwitchGM20BEmitIterated.h"

#include "nv_push_clb197.h"

void switch_gm20b_iterated_blend_common(struct nv_push *p, const struct switch_gm20b_blend_state *common)
{
    P_IMMD(p, NVB197, SET_BLEND_STATE_PER_TARGET, ENABLE_FALSE);
    P_MTHD(p, NVB197, SET_BLEND_SEPARATE_FOR_ALPHA);
    P_NVB197_SET_BLEND_SEPARATE_FOR_ALPHA(p, ENABLE_TRUE);
    P_NVB197_SET_BLEND_COLOR_OP(p, common->color_op);
    P_NVB197_SET_BLEND_COLOR_SOURCE_COEFF(p, common->color_source);
    P_NVB197_SET_BLEND_COLOR_DEST_COEFF(p, common->color_dest);
    P_NVB197_SET_BLEND_ALPHA_OP(p, common->alpha_op);
    P_NVB197_SET_BLEND_ALPHA_SOURCE_COEFF(p, common->alpha_source);
    P_IMMD(p, NVB197, SET_BLEND_ALPHA_DEST_COEFF, common->alpha_dest);
}

void switch_gm20b_iterated_blend(struct nv_push *p, const struct switch_gm20b_iterated_program *b)
{
    assert(b->count >= 1 && b->count <= SWITCH_GM20B_ITERATED_BLEND_MAX_PROGRAM);
    assert(b->constant_count <= SWITCH_GM20B_ITERATED_BLEND_MAX_CONSTANTS);
    switch_gm20b_iterated_blend_common(p, &b->common);
    for (uint32_t j = 0; 3 * j < b->constant_count; j++) {
        P_MTHD(p, NVB197, SET_ITERATED_BLEND_CONSTANT_RED(j));
        for (uint32_t c = 0; c < 3 && 3 * j + c < b->constant_count; c++)
            P_INLINE_DATA(p, b->constants[3 * j + c]);
    }
    P_IMMD(p, NVB197, SET_ITERATED_BLEND_OPTIMIZATION, NOOP_NEVER);
    P_IMMD(p, NVB197, LOAD_ITERATED_BLEND_INSTRUCTION_POINTER, 0);
    P_0INC(p, NVB197, LOAD_ITERATED_BLEND_INSTRUCTION);
    P_INLINE_ARRAY(p, b->program, (int)b->count);
    P_IMMD(p, NVB197, SET_ITERATED_BLEND_PASS, b->count);
    P_IMMD(p, NVB197, SET_ITERATED_BLEND, {.enable = ENABLE_TRUE, .alpha_enable = b->alpha});
}

void switch_gm20b_iterated_blend_off(struct nv_push *p)
{
    P_IMMD(p, NVB197, SET_ITERATED_BLEND, {.enable = ENABLE_FALSE, .alpha_enable = ALPHA_ENABLE_FALSE});
    P_IMMD(p, NVB197, SET_BLEND_STATE_PER_TARGET, ENABLE_TRUE);
}
