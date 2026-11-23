/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: a raw-path fragment program's float outputs to a 16-bit float
 * colour target, narrowed where NAK can see the half they hold
 * (nvk_switch_round.c).
 */

#ifndef NVK_SWITCH_ROUND_H
#define NVK_SWITCH_ROUND_H 1

#include <stdbool.h>
#include <stdint.h>

#include "nir.h"

/* GM20B's ROP converts an f32 colour to a 16-bit float target rounding
 * toward zero, as Apple's GPU stores a float output there at its default
 * textureWriteRoundingMode. Each 32-bit float output to colour attachment j
 * of `targets` (bit j) becomes f2f32(f2f16_rtz(value)), which stores what
 * the value would: NAK folds it into a value that is a half held in an f32
 * and removes it from any other (nak_nir_strip_half_rounding.c).
 */
bool switch_nvk_round_half_outputs(nir_shader *nir, uint32_t targets);

/* The 16-bit float colour attachments of the stage switch_nvk_compile_stage
 * compiles on this thread, 0 otherwise: nvk_lower_nir rounds them.
 */
extern _Thread_local uint32_t switch_nvk_half_targets_current;

#endif /* NVK_SWITCH_ROUND_H */
