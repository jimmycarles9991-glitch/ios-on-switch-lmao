/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

/*
 * Mesa's nouveau_copy_rect (src/nouveau/vulkan/nvk_cmd_copy.c, commit 77452b9092cc), adapted for
 * kits/SwitchGM20BEmit:
 *  - GM20B's copy class (b0b5) only: no Pascal origin methods, no Blackwell GOB kinds;
 *  - the caller's push, sized with SWITCH_GM20B_COPY_RECT_WORDS, instead of an NVK command buffer's;
 *  - plain surfaces (switch_gm20b_copy_surface) instead of nil and Vulkan types;
 *  - SET_SRC_ORIGIN and SET_DST_ORIGIN continue the block-size method run (they follow SET_*_LAYER): one header
 *    word fewer per block-linear side, as the raw path's copies already emit them.
 */

#include "../SwitchGM20BEmitCopy.h"

#include "nv_push_clb0b5.h"

static uint32_t
to_b0b5_remap_comp_size(uint32_t comp_size)
{
   assert(comp_size > 0 && comp_size <= 4);
   return comp_size - 1;
}

static uint32_t
to_b0b5_remap_num_comps(uint32_t num_comps)
{
   assert(num_comps > 0 && num_comps <= 4);
   return num_comps - 1;
}

void
switch_gm20b_copy_rect(struct nv_push *p, const struct switch_gm20b_copy *copy,
                       uint8_t data_transfer_type)
{
   uint32_t src_bw, dst_bw;
   if (copy->remap.comp_size > 0) {
      assert(copy->src.bpp % copy->remap.comp_size == 0);
      assert(copy->dst.bpp % copy->remap.comp_size == 0);
      uint32_t num_src_comps = copy->src.bpp / copy->remap.comp_size;
      uint32_t num_dst_comps = copy->dst.bpp / copy->remap.comp_size;

      /* When running with component remapping enabled, most X/Y dimensions
       * are in units of blocks.
       */
      src_bw = dst_bw = 1;

      P_IMMD(p, NVB0B5, SET_REMAP_COMPONENTS, {
         .dst_x = copy->remap.dst[0],
         .dst_y = copy->remap.dst[1],
         .dst_z = copy->remap.dst[2],
         .dst_w = copy->remap.dst[3],
         .component_size = to_b0b5_remap_comp_size(copy->remap.comp_size),
         .num_src_components = to_b0b5_remap_num_comps(num_src_comps),
         .num_dst_components = to_b0b5_remap_num_comps(num_dst_comps),
      });
   } else {
      /* When component remapping is disabled, dimensions are in units of
       * bytes (an implicit block width of 1B).
       */
      assert(copy->src.bpp == copy->dst.bpp);
      src_bw = copy->src.bpp;
      dst_bw = copy->dst.bpp;
   }

   assert(copy->extent_el.depth == 1 || copy->extent_el.array_len == 1);
   uint32_t layers = MAX2(copy->extent_el.depth, copy->extent_el.array_len);
   for (unsigned z = 0; z < layers; z++) {
      uint64_t src_addr = copy->src.base_addr;
      uint64_t dst_addr = copy->dst.base_addr;

      if (!copy->src.is_3d)
         src_addr += (z + copy->src.offset_el.a) * copy->src.array_stride;

      if (!copy->dst.is_3d)
         dst_addr += (z + copy->dst.offset_el.a) * copy->dst.array_stride;

      if (!copy->src.tiled) {
         src_addr += copy->src.offset_el.x * copy->src.bpp +
                     copy->src.offset_el.y * copy->src.row_stride;
      }

      if (!copy->dst.tiled) {
         dst_addr += copy->dst.offset_el.x * copy->dst.bpp +
                     copy->dst.offset_el.y * copy->dst.row_stride;
      }

      P_MTHD(p, NVB0B5, OFFSET_IN_UPPER);
      P_NVB0B5_OFFSET_IN_UPPER(p, src_addr >> 32);
      P_NVB0B5_OFFSET_IN_LOWER(p, src_addr & 0xffffffff);
      P_NVB0B5_OFFSET_OUT_UPPER(p, dst_addr >> 32);
      P_NVB0B5_OFFSET_OUT_LOWER(p, dst_addr & 0xffffffff);
      P_NVB0B5_PITCH_IN(p, copy->src.row_stride);
      P_NVB0B5_PITCH_OUT(p, copy->dst.row_stride);
      P_NVB0B5_LINE_LENGTH_IN(p, copy->extent_el.width * src_bw);
      P_NVB0B5_LINE_COUNT(p, copy->extent_el.height);

      uint32_t src_layout = 0, dst_layout = 0;
      if (copy->src.tiled) {
         P_MTHD(p, NVB0B5, SET_SRC_BLOCK_SIZE);
         P_NVB0B5_SET_SRC_BLOCK_SIZE(p, {
            .width = 0, /* Tiles are always 1 GOB wide */
            .height = copy->src.y_log2,
            .depth = copy->src.z_log2,
            .gob_height = GOB_HEIGHT_GOB_HEIGHT_FERMI_8,
         });
         /* We use the stride for copies because the copy hardware has no
          * concept of a tile width.  Instead, we just set the width to the
          * stride divided by bpp.
          */
         uint32_t src_stride_el = copy->src.row_stride / copy->src.bpp;
         P_NVB0B5_SET_SRC_WIDTH(p, src_stride_el * src_bw);
         P_NVB0B5_SET_SRC_HEIGHT(p, copy->src.extent_el.height);
         P_NVB0B5_SET_SRC_DEPTH(p, copy->src.extent_el.depth);
         if (copy->src.is_3d)
            P_NVB0B5_SET_SRC_LAYER(p, z + copy->src.offset_el.z);
         else
            P_NVB0B5_SET_SRC_LAYER(p, 0);

         P_NVB0B5_SET_SRC_ORIGIN(p, {
            .x = copy->src.offset_el.x * src_bw,
            .y = copy->src.offset_el.y
         });

         src_layout = NVB0B5_LAUNCH_DMA_SRC_MEMORY_LAYOUT_BLOCKLINEAR;
      } else {
         src_layout = NVB0B5_LAUNCH_DMA_SRC_MEMORY_LAYOUT_PITCH;
      }

      if (copy->dst.tiled) {
         P_MTHD(p, NVB0B5, SET_DST_BLOCK_SIZE);
         P_NVB0B5_SET_DST_BLOCK_SIZE(p, {
            .width = 0, /* Tiles are always 1 GOB wide */
            .height = copy->dst.y_log2,
            .depth = copy->dst.z_log2,
            .gob_height = GOB_HEIGHT_GOB_HEIGHT_FERMI_8,
         });
         /* We use the stride for copies because the copy hardware has no
          * concept of a tile width.  Instead, we just set the width to the
          * stride divided by bpp.
          */
         uint32_t dst_stride_el = copy->dst.row_stride / copy->dst.bpp;
         P_NVB0B5_SET_DST_WIDTH(p, dst_stride_el * dst_bw);
         P_NVB0B5_SET_DST_HEIGHT(p, copy->dst.extent_el.height);
         P_NVB0B5_SET_DST_DEPTH(p, copy->dst.extent_el.depth);
         if (copy->dst.is_3d)
            P_NVB0B5_SET_DST_LAYER(p, z + copy->dst.offset_el.z);
         else
            P_NVB0B5_SET_DST_LAYER(p, 0);

         P_NVB0B5_SET_DST_ORIGIN(p, {
            .x = copy->dst.offset_el.x * dst_bw,
            .y = copy->dst.offset_el.y
         });

         dst_layout = NVB0B5_LAUNCH_DMA_DST_MEMORY_LAYOUT_BLOCKLINEAR;
      } else {
         dst_layout = NVB0B5_LAUNCH_DMA_DST_MEMORY_LAYOUT_PITCH;
      }

      P_IMMD(p, NVB0B5, LAUNCH_DMA, {
         .data_transfer_type = data_transfer_type,
         .flush_enable = FLUSH_ENABLE_TRUE,
         .src_memory_layout = src_layout,
         .dst_memory_layout = dst_layout,
         .multi_line_enable = MULTI_LINE_ENABLE_TRUE,
         .remap_enable = copy->remap.comp_size > 0,
      });
   }
}
