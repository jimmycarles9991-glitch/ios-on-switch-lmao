/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Switch: NVK's device compile of one stage (nvk_switch_compile.c), for NVK's
 * own checks; the raw path compiles with no device (switch_compile.h).
 */

#ifndef NVK_SWITCH_COMPILE_H
#define NVK_SWITCH_COMPILE_H 1

#include "switch_compile.h"

#ifdef __cplusplus
extern "C" {
#endif

VkResult switch_nvk_compile_stage(VkDevice device,
                                  const struct switch_nvk_stage_info *info,
                                  struct switch_nvk_stage *out);
void switch_nvk_stage_finish(VkDevice device, struct switch_nvk_stage *stage);
VkResult switch_nvk_compile_ranked_stage(VkDevice device,
                                         const struct switch_nvk_stage_info *info,
                                         struct switch_nvk_stage *out);

#ifdef __cplusplus
}
#endif

#endif /* NVK_SWITCH_COMPILE_H */
