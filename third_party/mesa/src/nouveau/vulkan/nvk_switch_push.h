/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: the push words a raw-path render state holds constant
 * (switch_nvk_stage_info's const_push_mask and const_push_words) folded into
 * its code (nvk_switch_push.c).
 */

#ifndef NVK_SWITCH_PUSH_H
#define NVK_SWITCH_PUSH_H 1

#include <stdbool.h>
#include <stdint.h>

#include "nir.h"

/* Push word i is bytes 4i..4i+3 of the push-constant block (root table
 * 0x700 + 4i). A load of the block at a constant offset that reads only words
 * of `mask` becomes their value; any other load stays.
 */
struct switch_nvk_const_push {
   uint32_t mask;
   uint32_t words[32];
};

bool switch_nvk_fold_const_push(nir_shader *nir,
                                const struct switch_nvk_const_push *push);

/* The words of the stage switch_nvk_compile_stage compiles on this thread,
 * NULL otherwise: nvk_lower_nir folds them before lowering descriptors.
 */
extern _Thread_local const struct switch_nvk_const_push
   *switch_nvk_const_push_current;

#endif /* NVK_SWITCH_PUSH_H */
