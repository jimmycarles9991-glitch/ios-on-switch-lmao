/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: a raw-path fragment program split in two. What it computes from
 * per-draw values alone (constants and uniform reads) moves to a compute
 * prologue that writes the values the per-pixel code reads (the frontier);
 * the fragment program reads them from a constant buffer of its own. The raw
 * GM20B path batches compatible draws, one invocation per record. CB0 holds
 * {recordsVA:u64, count:u32, stride:u32}; CB1 holds shared shader constants.
 * Each 272-byte record holds push words at 0, frontierVA at 128 and eight
 * {VA:u64, bytes:u32, reserved:u32} views at 144. UBO k uses view 1+k.
 */

#ifndef NVK_SWITCH_SPLIT_H
#define NVK_SWITCH_SPLIT_H 1

#include <stdbool.h>
#include <stdint.h>

#include "nak.h"
#include "nir.h"
#include "nvk_switch_target.h"

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
#define SWITCH_SPLIT_BATCH_VERSION 1
#define SWITCH_SPLIT_BATCH_THREADS 32
#define SWITCH_SPLIT_BATCH_RECORD_BYTES 272
#define SWITCH_SPLIT_BATCH_BUFFERS 144

struct switch_nvk_split_layout {
   uint32_t frontier_bytes;
   uint8_t frontier_cbuf;
   uint8_t prologue_cbufs;
   uint8_t shader_data_cbuf;
   uint8_t ubo_count;
   uint8_t ubo_cbuf[SWITCH_SPLIT_MAX_UBOS];
   uint16_t batch_version;
   uint16_t batch_threads;
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

struct switch_nvk_split_cost {
   uint16_t version, reserved;
   uint32_t launch_cycles, drain_cycles;
   uint32_t prologue_warp_cycles, prologue_instructions;
   uint32_t saved_pixel_instructions, saved_pixel_cycles, frontier_values;
};

struct switch_nvk_split {
   struct switch_nvk_program fs;
   struct switch_nvk_program prologue;
   struct switch_nvk_split_layout layout;
   uint32_t qmd[64];
   /* the draw's pixels from which the split saves GPU time */
   uint32_t threshold_pixels;
   struct switch_nvk_split_cost cost;
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
switch_nvk_split_compile(const struct switch_nvk_target *t, const nir_shader *fs,
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
