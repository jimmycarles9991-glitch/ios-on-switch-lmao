/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: the stage compile's own handover (nvk_switch_stage.c), and the
 * ranked choice NVK's device compile shares with the offline one
 * (nvk_switch_rank.c).
 */

#ifndef NVK_SWITCH_STAGE_H
#define NVK_SWITCH_STAGE_H 1

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan_core.h>

struct nvk_cbuf;
struct nvk_shader;
struct switch_nvk_stage;
struct switch_nvk_stage_info;

extern _Thread_local bool switch_nvk_center_frag_depth_current;
extern _Thread_local bool switch_nvk_texs_f16_current;

/* A constant buffer as the raw path's pack names it (raw_pack.h PackCbufType) */
uint8_t switch_nvk_cbuf_code(const struct nvk_cbuf *cbuf);
/* A UBO descriptor's offset in its set, 0 for any other constant buffer */
uint32_t switch_nvk_cbuf_offset(const struct nvk_cbuf *cbuf);

/* What a stage compile hands over of a compiled shader */
void switch_nvk_stage_of(const struct nvk_shader *shader,
                         struct switch_nvk_stage *out);

struct switch_nvk_ranked_compiler {
   VkResult (*compile)(const void *ctx, const struct switch_nvk_stage_info *info,
                       struct switch_nvk_stage *out);
   void (*finish)(const void *ctx, struct switch_nvk_stage *stage);
   const void *ctx;
};

VkResult switch_nvk_ranked(const struct switch_nvk_ranked_compiler *c,
                           const struct switch_nvk_stage_info *info,
                           struct switch_nvk_stage *out);

#endif /* NVK_SWITCH_STAGE_H */
