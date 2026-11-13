/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: a split fragment program compiled (nvk_switch_split.h). It is kept
 * only where NAK's code for the per-pixel part has fewer instructions, no more
 * static cycles and no more registers than the whole program's.
 */

#include "nvk_switch_split.h"

#include "nvk_device.h"
#include "nvk_physical_device.h"
#include "nvk_shader.h"
#include "nvk_switch_capture.h"

#include "nak.h"
#include "util/u_math.h"

#include <stdlib.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#define SWITCH_SPLIT_CBUF_PARAMS 4
#define SWITCH_SPLIT_CBUF_FRONTIER 6

/* The GPU time a split draw adds whatever its size, in GPU cycles: its
 * prologue's launch and the compute segment's drain, which a command buffer
 * of one split draw pays alone, beside the prologue's own static cycles.
 */
#define SWITCH_SPLIT_DRAW_CYCLES 4000

/* GM20B issues 8 warp instructions a cycle (2 SMs of 4 schedulers): an
 * instruction a pixel costs 1/256 of a cycle where its warps keep them busy.
 */
#define SWITCH_SPLIT_PIXELS_PER_CYCLE 256

_Thread_local bool switch_nvk_split_current;

/* The host twin's proof (nvk_switch_split_proof.c): none on the device */
static unsigned
proof_samples(void)
{
#if defined(__APPLE__) && TARGET_OS_OSX
   const char *n = getenv("SWITCH_NVK_SPLIT_PROOF");
   return n ? (unsigned)strtoul(n, NULL, 0) : 0;
#else
   return 0;
#endif
}

static bool
copy_program(const struct nv_device_info *info,
             const struct nak_shader_bin *bin, const nir_shader *nir,
             struct switch_nvk_program *out)
{
   out->info = bin->info;
   out->code_bytes = bin->code_size;
   out->code = malloc(bin->code_size);
   if (out->code == NULL)
      return false;
   memcpy(out->code, bin->code, bin->code_size);
   if (nir->constant_data_size == 0)
      return true;
   out->data_bytes = align(nir->constant_data_size,
                           nvk_min_cbuf_alignment(info));
   out->data = calloc(1, out->data_bytes);
   if (out->data == NULL)
      return false;
   memcpy(out->data, nir->constant_data, nir->constant_data_size);
   return true;
}

static bool
compile_program(const struct switch_nvk_target *t, nir_shader *nir,
                nir_variable_mode robust2_modes,
                const struct nak_fs_key *fs_key,
                struct switch_nvk_program *out)
{
   struct nak_shader_bin *bin =
      nak_compile_shader(nir, false, t->nak, robust2_modes, fs_key, false);
   if (bin == NULL)
      return false;
   const bool copied = copy_program(t->info, bin, nir, out);
   nak_shader_bin_destroy(bin);
   return copied;
}

static void
fill_qmd(const struct switch_nvk_target *t, struct switch_nvk_split *split)
{
   struct nak_qmd_info info = {
      .global_size = {1, 1, 1},
      .num_cbufs = split->prologue.cbuf_count,
   };
   for (uint32_t c = 0; c < info.num_cbufs; c++)
      info.cbufs[c] = (struct nak_qmd_cbuf){.index = c, .size = 256};
   nak_fill_qmd(t->info, &split->prologue.info, &info, split->qmd,
                sizeof(split->qmd));
}

static bool
wins(const struct nak_shader_info *whole, const struct nak_shader_info *part)
{
   return part->num_instrs < whole->num_instrs &&
          part->num_static_cycles <= whole->num_static_cycles &&
          part->num_gprs <= whole->num_gprs &&
          part->slm_size <= whole->slm_size;
}

/* The pixels from which a draw's saving covers what its split adds */
static uint32_t
threshold_pixels(const struct nak_shader_info *whole,
                 const struct switch_nvk_split *split)
{
   const uint64_t saved = whole->num_instrs - split->fs.info.num_instrs;
   const uint64_t cost =
      SWITCH_SPLIT_DRAW_CYCLES + split->prologue.info.num_static_cycles;
   return DIV_ROUND_UP(cost * SWITCH_SPLIT_PIXELS_PER_CYCLE, saved);
}

static void
name_cbufs(const struct nvk_cbuf_map *map, struct switch_nvk_split *split)
{
   const struct switch_nvk_split_layout *l = &split->layout;
   split->fs.cbuf_count = map->cbuf_count + 1;
   for (uint32_t c = 0; c < map->cbuf_count; c++)
      split->fs.cbuf_types[c] = switch_nvk_cbuf_code(&map->cbufs[c]);
   split->fs.cbuf_types[l->frontier_cbuf] = SWITCH_SPLIT_CBUF_FRONTIER;

   split->prologue.cbuf_count = l->prologue_cbufs;
   split->prologue.cbuf_types[SWITCH_SPLIT_PARAMS_CBUF] = SWITCH_SPLIT_CBUF_PARAMS;
   if (l->shader_data_cbuf)
      split->prologue.cbuf_types[l->shader_data_cbuf] = NVK_CBUF_TYPE_SHADER_DATA;
}

struct switch_nvk_split *
switch_nvk_split_compile(const struct switch_nvk_target *t, const nir_shader *fs,
                         const struct nvk_cbuf_map *map,
                         nir_variable_mode robust2_modes,
                         const struct nak_fs_key *fs_key,
                         const struct nak_shader_info *whole,
                         uint64_t whole_id)
{
   struct switch_nvk_split_nir nir;
   if (!switch_nvk_split_nir(fs, map, &nir))
      return NULL;

   const struct switch_nvk_split_proof proof = {
      .samples = proof_samples(),
      .whole_id = whole_id,
      .nak = t->nak,
      .robust2_modes = robust2_modes,
      .fs_key = fs_key,
   };
   if (proof.samples > 0)
      switch_nvk_split_prove(fs, map, &nir, &proof);

   struct switch_nvk_split *split = calloc(1, sizeof(*split));
   bool ok = split != NULL;
   if (ok) {
      split->layout = nir.layout;
      ok = compile_program(t, nir.fs, robust2_modes, fs_key, &split->fs) &&
           compile_program(t, nir.prologue, 0, NULL, &split->prologue);
   }
   ralloc_free(nir.fs);
   ralloc_free(nir.prologue);
   if (!ok || !wins(whole, &split->fs.info) ||
       split->prologue.info.num_static_cycles > UINT32_MAX) {
      switch_nvk_split_free(split);
      return NULL;
   }
   name_cbufs(map, split);
   fill_qmd(t, split);
   split->threshold_pixels = threshold_pixels(whole, split);
   /* Fixed launch/drain costs are conservative heuristics, not device measurements. */
   split->cost = (struct switch_nvk_split_cost) {
      .version = 1,
      .launch_cycles = SWITCH_SPLIT_DRAW_CYCLES,
      .drain_cycles = SWITCH_SPLIT_DRAW_CYCLES,
      .prologue_warp_cycles = split->prologue.info.num_static_cycles,
      .prologue_instructions = split->prologue.info.num_instrs,
      .saved_pixel_instructions = whole->num_instrs - split->fs.info.num_instrs,
      .saved_pixel_cycles = MIN2(whole->num_static_cycles - split->fs.info.num_static_cycles, UINT32_MAX),
      .frontier_values = split->layout.frontier_values,
   };
   return split;
}

void
switch_nvk_split_free(struct switch_nvk_split *split)
{
   if (split == NULL)
      return;
   free(split->fs.code);
   free(split->fs.data);
   free(split->prologue.code);
   free(split->prologue.data);
   free(split);
}

static uint64_t
fnv(uint64_t h, const void *bytes, size_t n)
{
   const uint8_t *b = bytes;
   for (size_t i = 0; i < n; i++)
      h = (h ^ b[i]) * 0x100000001b3ull;
   return h;
}

uint64_t
switch_nvk_program_id(const struct nak_shader_info *info, const void *code,
                      uint32_t code_bytes, const void *data,
                      uint32_t data_bytes)
{
   uint64_t id = fnv(0xcbf29ce484222325ull, info->hdr, 80);
   id = fnv(id, code, code_bytes);
   return fnv(id, data, data_bytes);
}
