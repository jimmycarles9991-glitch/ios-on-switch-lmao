/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: a raw-path fragment program rounds what it writes to a 16-bit
 * float colour target to nearest even itself (nvk_switch_round.c).
 */

#ifndef NVK_SWITCH_ROUND_H
#define NVK_SWITCH_ROUND_H 1

#include <stdbool.h>
#include <stdint.h>

#include "nir.h"

/* GM20B's ROP converts an f32 colour to a 16-bit float target rounding
 * toward zero, and the 3D class has no rounding control; Metal rounds to
 * nearest even. Each 32-bit float output to colour attachment j of
 * `targets` (bit j) becomes f2f32(f2f16_rtne(value)), which the ROP then
 * stores exactly, denormals included.
 */
bool switch_nvk_round_half_outputs(nir_shader *nir, uint32_t targets);

/* The 16-bit float colour attachments of the stage switch_nvk_compile_stage
 * compiles on this thread, 0 otherwise: nvk_lower_nir rounds them.
 */
extern _Thread_local uint32_t switch_nvk_half_targets_current;

#endif /* NVK_SWITCH_ROUND_H */
