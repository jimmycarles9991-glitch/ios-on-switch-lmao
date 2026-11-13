// SPDX-License-Identifier: MIT
#include "SwitchGM20BEmitRaster.h"

#include "nv_push_clb197.h"

void switch_gm20b_constant_color(struct nv_push *p, const uint32_t rgba[4])
{
    P_MTHD(p, NVB197, SET_CONSTANT_COLOR_RENDERING);
    P_NVB197_SET_CONSTANT_COLOR_RENDERING(p, ENABLE_TRUE);
    P_NVB197_SET_CONSTANT_COLOR_RENDERING_RED(p, rgba[0]);
    P_NVB197_SET_CONSTANT_COLOR_RENDERING_GREEN(p, rgba[1]);
    P_NVB197_SET_CONSTANT_COLOR_RENDERING_BLUE(p, rgba[2]);
    P_NVB197_SET_CONSTANT_COLOR_RENDERING_ALPHA(p, rgba[3]);
}

void switch_gm20b_constant_color_off(struct nv_push *p)
{
    P_IMMD(p, NVB197, SET_CONSTANT_COLOR_RENDERING, ENABLE_FALSE);
}

void switch_gm20b_fill_via_triangle(struct nv_push *p, uint32_t mode)
{
    P_IMMD(p, NVB197, SET_FILL_VIA_TRIANGLE, mode);
}
