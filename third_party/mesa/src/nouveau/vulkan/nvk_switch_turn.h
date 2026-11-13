/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: a fragment program drawn into a turned plane (the raw GM20B path's
 * rotated drawables, raw/runtime/raw_rotation.h) sees what Metal's frame
 * shows it (nvk_switch_turn.c).
 */

#ifndef NVK_SWITCH_TURN_H
#define NVK_SWITCH_TURN_H 1

#include <stdbool.h>
#include <stdint.h>

#include "nir.h"

/* The pass rasterises the plane, so the hardware's fragment position, point
 * coordinate, sample position and derivatives run along the plane's axes.
 * The frame's are a signed swap of them: for a plane value p, the frame's
 * f.x = sx (swap ? p.y : p.x) + tx and f.y = sy (swap ? p.x : p.y) + ty, and a
 * derivative's the same without the offsets. The words live in the root
 * table at `root_offset` (raw_rotation.h TurnWords): swap (0 or 1), sx, sy,
 * tx, ty, then the point coordinate's offsets (1 where the axis's sign is
 * negative), floats but the first.
 *
 * Left as they are: a derivative pair whose only use is fwidth (|ddx| + |ddy|
 * of one value, the same under any signed swap), a position whose only use is
 * an image access's coordinate (an attachment the program fetches through
 * the interlock is laid out as the plane), implicit-LOD sampling, a sample's
 * position (the root table holds the frame's). A program none of whose reads
 * change compiles to the same code.
 */
bool switch_nvk_turn_fragment(nir_shader *nir, uint32_t root_offset);

/* Whether the stage switch_nvk_compile_stage compiles on this thread is the
 * turned form: nvk_lower_nir turns it, nvk_populate_fs_key keys it.
 */
extern _Thread_local bool switch_nvk_turned_current;

#endif /* NVK_SWITCH_TURN_H */
