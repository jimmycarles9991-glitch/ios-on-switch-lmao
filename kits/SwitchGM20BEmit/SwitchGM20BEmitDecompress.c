// SPDX-License-Identifier: MIT
#include "SwitchGM20BEmitDecompress.h"

#include "nv_push_clb197.h"

void switch_gm20b_decompress_color(struct nv_push *p, uint32_t mrt, uint32_t layer)
{
    assert(mrt < 8 && layer < (1u << 16));
    P_MTHD(p, NVB197, DECOMPRESS_SURFACE);
    P_NVB197_DECOMPRESS_SURFACE(p, {.mrt_select = mrt, .rt_array_index = layer});
}

void switch_gm20b_decompress_zeta(struct nv_push *p, bool depth, bool stencil)
{
    P_MTHD(p, NVB197, DECOMPRESS_ZETA_SURFACE);
    P_NVB197_DECOMPRESS_ZETA_SURFACE(p, {.z_enable = depth, .stencil_enable = stencil});
}
