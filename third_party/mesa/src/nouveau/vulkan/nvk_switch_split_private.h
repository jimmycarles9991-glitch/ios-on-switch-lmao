/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: what the split's NIR pass shares with its prologue builder
 * (nvk_switch_split_nir.c, nvk_switch_split_prologue.c).
 */

#ifndef NVK_SWITCH_SPLIT_PRIVATE_H
#define NVK_SWITCH_SPLIT_PRIVATE_H 1

#include "nvk_switch_split.h"

#include "nvk_cmd_buffer.h"
#include "nvk_shader.h"

#include "util/bitset.h"

#define PUSH_START nvk_root_descriptor_offset(push)

struct split {
   nir_shader *fs;
   nir_function_impl *impl;
   const struct nvk_cbuf_map *map;
   BITSET_WORD *per_draw, *frontier, *computed;
   uint32_t *slot;
   uint32_t frontier_bytes, frontier_values, computed_instrs;
   int8_t ubo_of_cbuf[16];
   struct switch_nvk_split_layout *layout;
};

/* A frontier value's slot per component: 4 bytes, 8 for a 64-bit value */
static inline unsigned
slot_bytes(const nir_def *def)
{
   return def->bit_size == 64 ? 8 : 4;
}

/* The prologue's constant buffers for what it reads: false with more uniform
 * buffers than a launch binds
 */
bool switch_split_lay_out_cbufs(struct split *s);
nir_shader *switch_split_build_prologue(struct split *s);

#endif /* NVK_SWITCH_SPLIT_PRIVATE_H */
