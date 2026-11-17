/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#ifndef NVK_ROOT_TABLE_H
#define NVK_ROOT_TABLE_H 1

#include "nvk_private.h"
#include "nvk_descriptor_types.h"

#include "nak.h"
#include "nv_device_info.h"
#include "clc597.h"

#include <stddef.h>

/** Root descriptor table.  This gets pushed to the GPU directly */
struct nvk_root_descriptor_table {
   union {
      struct {
         struct {
            uint32_t base_vertex;
            uint32_t base_instance;
         } vs;
         struct {
            uint32_t group_count[3];
         } mesh;
         uint32_t draw_index;
         uint32_t view_index;
         struct nak_sample_location sample_locations[NVK_MAX_SAMPLES];
         struct nak_sample_mask sample_masks[NVK_MAX_SAMPLES];
         uint32_t __padding;
      } draw;
      struct {
         uint32_t base_group[3];
         uint32_t group_count[3];
      } cs;
   };

   /* Descriptor set addresses */
   struct nvk_buffer_address sets[NVK_MAX_SETS];

   /* For each descriptor set, the index in dynamic_buffers where that set's
    * dynamic buffers start.
    */
   uint8_t set_dynamic_buffer_start[NVK_MAX_SETS];

   uint64_t printf_buffer_addr;

   /* Fragment shader interlock's ticket buffer: 256 tickets per GPC */
   uint64_t interlock_ticket_buffer;

   /* Switch: a turned pass's frame of its plane (nvk_switch_turn.h) */
   uint32_t switch_turn[8];

   /* enfore total structure alignment to 0x100 as needed pre pascal */
   uint8_t __padding[0x78];

   /*
    * Arrays with dynamic (shader-provided) indices need to fit in a single
    * 256-byte bank for gpus with ROOT_TABLE. We place them here after the
    * padding so they're appropriately aligned.
    */

   /* Dynamic buffer bindings (swizzled form of nvk_buffer_descriptor) */
   uint32_t dynamic_buffers[4][NVK_MAX_DYNAMIC_BUFFERS];

   /* Client push constants */
   uint8_t push[NVK_MAX_PUSH_SIZE];
};

/* helper macro for computing root descriptor byte offsets */
#define nvk_root_descriptor_offset(member)\
   offsetof(struct nvk_root_descriptor_table, member)

/* Push constants should be aligned properly */
static_assert(nvk_root_descriptor_offset(push) % 8 == 0,
              "Push constants should be aligned properly");
static_assert(nvk_root_descriptor_offset(switch_turn) == 0x268,
              "raw/runtime/raw_rotation.h kRootTurn");
static_assert(nvk_root_descriptor_offset(dynamic_buffers) == 0x300,
              "the turn words take the padding's room");

#define nvk_hw_root_table_index(member)\
   (nvk_root_descriptor_offset(member) / NVK_HW_ROOT_TABLE_SIZE)
#define nvk_hw_root_table_offset(member)\
   (nvk_root_descriptor_offset(member) % NVK_HW_ROOT_TABLE_SIZE)

static inline bool nvk_use_hw_root_table(const struct nv_device_info *info,
                                         bool is_gfx)
{
   return is_gfx && info->cls_eng3d >= TURING_A;
}

#endif
