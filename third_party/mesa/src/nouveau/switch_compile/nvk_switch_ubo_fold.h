/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: the bytes of one uniform buffer a raw-path fragment variant holds
 * constant (switch_nvk_stage_info's ubo_fold_desc, ubo_fold_mask and
 * ubo_fold_bytes) folded into its code. Core Animation's generic programs
 * branch at run time on a config at the start of such a buffer; with its
 * bytes folded, NIR drops the branches the config does not take.
 */

#ifndef NVK_SWITCH_UBO_FOLD_H
#define NVK_SWITCH_UBO_FOLD_H 1

#include <stdbool.h>
#include <stdint.h>

#include "nir.h"

struct nvk_cbuf_map;

/* The buffer is the constant buffer of map's NVK_CBUF_TYPE_UBO_DESC key
 * {set 0, desc}; byte i of it is bytes[i] wherever bit i of mask is set.
 */
struct switch_nvk_ubo_fold {
   uint32_t desc;
   uint64_t mask;
   uint8_t bytes[64];
};

/* A read of that constant buffer at a constant offset (load_ubo, or ldc_nv
 * once lowered) becomes constant where every observed bit is held. Other
 * components stay run-time reads. The constant buffer list stays the
 * unfolded compile's, as the raw runtime refuses a variant whose constant
 * buffers differ. A second fold of the same shader changes nothing.
 */
bool switch_nvk_fold_ubo(nir_shader *nir, const struct nvk_cbuf_map *map,
                         const struct switch_nvk_ubo_fold *fold);

/* The fold of the stage switch_nvk_compile_stage compiles on this thread,
 * NULL otherwise: switch_nvk_lower folds it once UBO reads are lowered.
 */
extern _Thread_local const struct switch_nvk_ubo_fold
   *switch_nvk_ubo_fold_current;

#endif /* NVK_SWITCH_UBO_FOLD_H */
