/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: a raw-path fragment program split in two. What it computes from
 * per-draw values alone (constants and uniform reads) moves to a compute
 * prologue that writes the values the per-pixel code reads (the frontier);
 * the fragment program reads them from a constant buffer of its own. The raw
 * GM20B path launches a split draw's prologue, one thread with the draw's
 * constant buffers bound, in one compute segment per command buffer ahead of
 * its 3D work.
 *
 * The prologue's constant buffers: 0 the draw's root-table push words
 * [0x700, 0x780) at 0 and its frontier's address (u64) at 128; 1 to ubo_count
 * the uniform buffers ubo_cbuf names, as the draw binds them; then the shader
 * data where shader_data_cbuf is not 0.
 */

#ifndef NVK_SWITCH_SPLIT_H
#define NVK_SWITCH_SPLIT_H 1

#include <stdbool.h>
#include <stdint.h>

#include "nak.h"
#include "nir.h"

#ifdef __cplusplus
extern "C" {
#endif

struct nvk_cbuf_map;
struct nvk_device;

#define SWITCH_SPLIT_MAX_UBOS 6
#define SWITCH_SPLIT_PUSH_BYTES 128
#define SWITCH_SPLIT_PARAMS_CBUF 0
#define SWITCH_SPLIT_FRONTIER_ADDRESS 128
#define SWITCH_SPLIT_MAX_FRONTIER_BYTES 4096

struct switch_nvk_split_layout {
   uint32_t frontier_bytes;
   uint8_t frontier_cbuf;
   uint8_t prologue_cbufs;
   uint8_t shader_data_cbuf;
   uint8_t ubo_count;
   uint8_t ubo_cbuf[SWITCH_SPLIT_MAX_UBOS];
   uint32_t frontier_values;
   uint32_t prologue_instrs;
};

struct switch_nvk_split_nir {
   nir_shader *fs;
   nir_shader *prologue;
   struct switch_nvk_split_layout layout;
};

/* `fs` as NVK lowered it for NAK (before nak_compile_shader), whose constant
 * buffers are `map`. False: nothing to move, or no room for the frontier's
 * constant buffer. `fs` is left as it was; out's shaders are new.
 */
bool switch_nvk_split_nir(const nir_shader *fs, const struct nvk_cbuf_map *map,
                          struct switch_nvk_split_nir *out);

struct switch_nvk_program {
   struct nak_shader_info info;
   void *code;
   uint32_t code_bytes;
   void *data;
   uint32_t data_bytes;
   uint8_t cbuf_types[16];
   uint32_t cbuf_count;
};

struct switch_nvk_split {
   struct switch_nvk_program fs;
   struct switch_nvk_program prologue;
   struct switch_nvk_split_layout layout;
   uint32_t qmd[64];
   /* the draw's pixels from which the split saves GPU time */
   uint32_t threshold_pixels;
};

/* A program as raw/pack/mkpack.py names it: FNV-1a 64 of its 80-byte header,
 * its code and its shader data
 */
uint64_t switch_nvk_program_id(const struct nak_shader_info *info,
                               const void *code, uint32_t code_bytes,
                               const void *data, uint32_t data_bytes);

/* The split of a fragment stage, compiled as the stage is (fs_key,
 * robustness). NULL where nothing moves or the per-pixel part is no cheaper
 * than `whole`, the stage's own program (`whole_id` names it in the proof).
 */
struct switch_nvk_split *
switch_nvk_split_compile(struct nvk_device *dev, const nir_shader *fs,
                         const struct nvk_cbuf_map *map,
                         nir_variable_mode robust2_modes,
                         const struct nak_fs_key *fs_key,
                         const struct nak_shader_info *whole,
                         uint64_t whole_id);
void switch_nvk_split_free(struct switch_nvk_split *split);

/* Whether the fragment stage NVK compiles on this thread is split: only where
 * switch_nvk_compile_stage asks (switch_nvk_stage_info.split), never for
 * metal2vulkan's own pipelines, which never run a split
 */
extern _Thread_local bool switch_nvk_split_current;

/* The host's proof that `split` computes what `fs` does: both folded to
 * constants by NIR's constant folder for `samples` pixels, at the split and
 * after NAK's NIR passes (macOS only, SWITCH_NVK_SPLIT_PROOF=<samples>). One
 * line on stderr.
 */
struct switch_nvk_split_proof {
   unsigned samples;
   uint64_t whole_id;
   const struct nak_compiler *nak;
   nir_variable_mode robust2_modes;
   const struct nak_fs_key *fs_key;
};

void switch_nvk_split_prove(const nir_shader *fs,
                            const struct nvk_cbuf_map *map,
                            const struct switch_nvk_split_nir *split,
                            const struct switch_nvk_split_proof *proof);

#ifdef __cplusplus
}
#endif

#endif /* NVK_SWITCH_SPLIT_H */
