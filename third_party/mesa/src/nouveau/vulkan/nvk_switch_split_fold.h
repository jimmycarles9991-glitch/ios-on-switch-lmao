/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: a shader folded to constants through NIR's constant folder
 * (nvk_switch_split_fold.c), the split's proof's engine.
 */

#ifndef NVK_SWITCH_SPLIT_FOLD_H
#define NVK_SWITCH_SPLIT_FOLD_H 1

#include <stdbool.h>
#include <stdint.h>

#include "nir.h"

struct switch_fold_model {
   void *ctx;
   uint64_t sample;
   uint32_t workgroup_id[3], local_id[3];
   bool check_alignment;
   /* The byte at `at` of constant buffer `cbuf`, or of global memory: -1
    * where the model has none (the fold fails)
    */
   int (*read)(void *ctx, bool global, uint64_t cbuf, uint64_t at);
   bool (*write)(void *ctx, uint64_t at, uint8_t byte);
};

/* `s` folded in place until nothing changes: false where the model refused
 * an access. Intrinsics without a result but store_global stay.
 */
bool switch_fold(nir_shader *s, const struct switch_fold_model *m);

#endif /* NVK_SWITCH_SPLIT_FOLD_H */
