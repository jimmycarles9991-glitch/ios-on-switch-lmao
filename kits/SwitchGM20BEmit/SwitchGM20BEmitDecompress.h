// SPDX-License-Identifier: MIT
// In-place decompression of a bound target (clb197.h DECOMPRESS_SURFACE 0x02e0, DECOMPRESS_ZETA_SURFACE 0x11fc): the ROP
// writes every compressed tile of colour target `mrt`'s layer `layer`, or of the zeta's depth and stencil, back as plain
// texels in the same memory, so a reader that does not know the tiles' compression state (an m2v twin bound through a
// plain kind, the CPU) reads the texels. The target must be bound as it was rendered (SET_COLOR_TARGET_* or SET_ZT_*);
// the caller orders the decompression after the target's last write and its readers after it, as for any ROP write.
#pragma once

#include "../SwitchGM20B/SwitchGM20B3D.h"
#include "nv_push.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_GM20B_DECOMPRESS_WORDS 2u

void switch_gm20b_decompress_color(struct nv_push *p, uint32_t mrt, uint32_t layer);

void switch_gm20b_decompress_zeta(struct nv_push *p, bool depth, bool stencil);

#ifdef __cplusplus
}
#endif
