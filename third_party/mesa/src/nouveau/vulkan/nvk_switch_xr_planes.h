/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: a fragment stage that samples RGB10A8_2P_XR10 textures held in
 * Metal's two planes (the raw GM20B path's raw/runtime/raw_xr_pass.h). The raw
 * path binds such a slot's header pair: plane 0's words as RGB10A2Unorm,
 * swizzled so that red, green and blue read the red, green and blue codes over
 * 1023, and plane 1's alpha bytes as R8Unorm at the next header index.
 */

#ifndef NVK_SWITCH_XR_PLANES_H
#define NVK_SWITCH_XR_PLANES_H 1

#include <stdbool.h>
#include <stdint.h>

#include "nir.h"

/* A texture instruction's backend flag between this pass and
 * nvk_nir_lower_descriptors: its image index is one past its descriptor's,
 * the slot's alpha plane. The descriptor lowering clears it.
 */
#define SWITCH_NVK_TEX_ALPHA_PLANE (1u << 31)

/* Each sample, fetch and gather of a set-0 binding in `bindings` (bit b:
 * binding b) reads both planes and returns what Metal's read of the format
 * returns: each colour lane (c - 384) / 510 of its code c and alpha the byte
 * over 255, the hardware's filter of the codes put on Metal's grid of 16 steps
 * a code (a nearest read's own code), each rounded to the nearest half (no
 * value on the grid is a tie). Plane 0 is read in f32 whatever the
 * instruction's type, as a half cannot hold c / 1023. A gather of alpha reads
 * plane 1 alone. Size and level queries read plane 0.
 */
bool switch_nvk_xr_planes(nir_shader *nir, uint64_t bindings);

/* The set-0 bindings switch_nvk_compile_stage compiles the stage with on this
 * thread (0: none): nvk_lower_nir joins them, nvk_populate_fs_key keys them.
 */
extern _Thread_local uint64_t switch_nvk_xr_plane_bindings_current;

#endif /* NVK_SWITCH_XR_PLANES_H */
