/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: one stage of a render state compiled as metal2vulkan's pipeline
 * libraries compile it, or a kernel as its compute pipeline compiles it, with
 * no pipeline, for the raw GM20B path to make the state's record from
 * (nvk_switch_compile.c). The plugin finds it with dlsym.
 */

#ifndef NVK_SWITCH_COMPILE_H
#define NVK_SWITCH_COMPILE_H 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan_core.h>

#include "nvk_switch_split.h"

#ifdef __cplusplus
extern "C" {
#endif

/* raw/runtime/raw_direct_nvk.h copies these structs field for field (the
 * plugin has no Vulkan headers): a field added here goes into its copies too,
 * which switch_nvk_compile_abi's sizes hold them to
 */
struct switch_nvk_stage_info {
   const uint32_t *spirv;
   size_t spirv_bytes;
   const char *entry;
   VkShaderStageFlagBits stage;
   const VkSpecializationInfo *specialization;
   /* The layout the library is created with: its set layouts, its push range
    * for the stage and its flags (independent sets) all reach the code.
    */
   VkPipelineLayout layout;
   VkPipelineCreateFlags2KHR pipeline_flags;
   /* The library creation's chain and the stage's: the robustness they ask
    * for, as vk_pipeline_robustness_state_fill reads it. Either may be NULL.
    */
   const void *pipeline_pnext;
   const void *stage_pnext;
   /* The fragment library's multisample state */
   bool sample_shading;
   /* A fragment stage's split for the raw path too (nvk_switch_split.h): it
    * compiles the stage about twice over, which the device does off the
    * state's creation
    */
   bool split;
   /* The colour attachments of a 16-bit float format (bit j: attachment j),
    * whose float outputs the fragment program rounds to nearest even
    * (nvk_switch_round.h)
    */
   uint32_t half_targets;
   /* Root push words [0x700, 0x780) constant across draws (bit i: word i). */
   uint32_t const_push_mask;
   uint32_t const_push_words[32];
   uint64_t center_frag_depth;
   const struct switch_nvk_ir *semantic;
   const void *portable_context;
   bool (*portable)(const void *context, const uint32_t **words, size_t *bytes);
   /* A fragment stage's fetches rounded to half take TEXS's F16 forms (a chip
    * fact the pack builder holds: raw_chip_facts.h AssumeTexsF16)
    */
   bool texs_f16;
   /* A fragment stage drawn into a turned plane (nvk_switch_turn.h) */
   bool turned;
   /* A fragment stage's set-0 bindings of two-plane RGB10A8_2P_XR10 textures
    * (bit b: binding b), each read through its header pair
    * (nvk_switch_xr_planes.h)
    */
   uint64_t xr_plane_bindings;
};

extern _Thread_local bool switch_nvk_center_frag_depth_current;
extern _Thread_local bool switch_nvk_texs_f16_current;

struct switch_nvk_stage {
   uint8_t header[80];
   const void *code;
   uint32_t code_bytes;
   const void *data;
   uint32_t data_bytes;
   uint8_t cbuf_types[16]; /* switch_nvk_cbuf_code, the pack's PackProgram.cbufType */
   uint32_t cbuf_count;
   uint8_t num_gprs;
   bool early_fragment_tests;
   bool writes_depth;
   bool post_depth_coverage;
   /* A fragment stage's split for the raw path (nvk_switch_split.h), NULL
    * where it has none; owned with code and data
    */
   const struct switch_nvk_split *split;
   void *owner; /* holds code and data until switch_nvk_stage_finish */
   uint32_t interlock_control; /* SET_PIXEL_SHADER_INTERLOCK_CONTROL */
   uint64_t uses_sample_shading;
   uint32_t semantic_version;
   uint8_t bound_textures; /* nak_shader_info's: SET_BINDLESS_TEXTURE + 1, 0 none */
   /* What nak_fill_qmd reads of a compute program's nak_shader_info, and
    * each NVK_CBUF_TYPE_DESC_SET constant buffer's set
    */
   uint8_t num_barriers;
   uint16_t local_size[3];
   uint16_t smem_size;
   uint32_t slm_size, crs_size;
   uint8_t cbuf_sets[16];
};

VkResult switch_nvk_compile_stage(VkDevice device,
                                  const struct switch_nvk_stage_info *info,
                                  struct switch_nvk_stage *out);
void switch_nvk_stage_finish(VkDevice device, struct switch_nvk_stage *stage);

struct switch_nvk_stage_metrics {
   uint32_t version, code_bytes, instructions, gprs, max_warps;
   uint32_t scratch_bytes, memory_spills, memory_fills, register_spills, register_fills;
   uint64_t static_cycles;
};

bool switch_nvk_stage_metrics(const struct switch_nvk_stage *stage,
                              struct switch_nvk_stage_metrics *out, size_t bytes);
VkResult switch_nvk_compile_ranked_stage(VkDevice device,
                                         const struct switch_nvk_stage_info *info,
                                         struct switch_nvk_stage *out);

/* A stage's split as the raw path records one: its per-pixel program and its
 * prologue as stages (the prologue's header zeros, as programs.bin holds a
 * kernel's; neither owns anything), its layout, QMD and threshold
 */
struct switch_nvk_split_parts {
   struct switch_nvk_stage fs;
   struct switch_nvk_stage prologue;
   struct switch_nvk_split_layout layout;
   uint32_t qmd[64];
   uint32_t threshold_pixels;
   struct switch_nvk_split_cost cost;
};

void switch_nvk_split_parts(const struct switch_nvk_split *split,
                            struct switch_nvk_split_parts *out);

/* sizeof switch_nvk_stage_info, switch_nvk_stage and switch_nvk_split_parts:
 * the plugin's copies of them are checked against these
 */
void switch_nvk_compile_abi(uint32_t sizes[3]);

#ifdef __cplusplus
}
#endif

#endif /* NVK_SWITCH_COMPILE_H */
