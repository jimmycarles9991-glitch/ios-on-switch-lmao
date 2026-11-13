/* SPDX-License-Identifier: MIT */
#include "nvk_device.h"
#include "nvk_mme.h"
#include "nvk_physical_device.h"
#include "nvk_shader.h"
#include "nvk_switch_capture.h"
#include "nvk_switch_compile.h"
#include "nvk_switch_push_test.h"
#include "vk_alloc.h"

#include "clb197.h"

#include <stdlib.h>

#define CONFLICT_MODE(control) ((control) & 3)

/* What a pixel-interlocked fragment shader's push writes: the tile
 * coalescer's conflict detection, the anti-alias macro's minSampleShading
 * (passes = samples at 1.0), and the raw stage's conflict detection
 */
struct interlocked {
   uint32_t control, anti_alias, stage_control;
};

static struct interlocked
fragment_push(struct nvk_device *dev, bool reads_sample_id,
              bool sample_shading_enable)
{
   struct nvk_shader *shader = calloc(1, sizeof(*shader));
   shader->info.stage = MESA_SHADER_FRAGMENT;
   shader->info.fs.interlock_mode = NAK_FS_INTERLOCK_MODE_PIXEL;
   shader->info.fs.uses_sample_shading = reads_sample_id;
   shader->sample_shading_enable = sample_shading_enable;
   shader->min_sample_shading = 1;
   shader->hdr_addr = 0x1000;
   struct interlocked out = { NO_VALUE, NO_VALUE, NO_VALUE };
   if (nvk_shader_fill_push(dev, shader, vk_default_allocator()) != VK_SUCCESS) {
      check(false, "nvk_shader_fill_push");
      free(shader);
      return out;
   }
   out.control = last_write(shader->push_dw, shader->push_dw_count,
                            NVB197_SET_PIXEL_SHADER_INTERLOCK_CONTROL);
   out.anti_alias = last_write(shader->push_dw, shader->push_dw_count,
                               NVB197_CALL_MME_MACRO(NVK_MME_SET_ANTI_ALIAS));
   struct switch_nvk_stage stage;
   switch_nvk_stage_of(shader, &stage);
   out.stage_control = stage.interlock_control;
   vk_free(vk_default_allocator(), shader->push_dw);
   free(shader);
   return out;
}

int
main(int argc, char **argv)
{
   (void)argc;
   (void)argv;
   struct nvk_physical_device *pdev = calloc(1, sizeof(*pdev));
   struct nvk_device *dev = calloc(1, sizeof(*dev));
   pdev->info.cls_eng3d = MAXWELL_B;
   dev->vk.physical = &pdev->vk;
   dev->vk.enabled_features.fragmentShaderPixelInterlock = true;

   const uint32_t sample = NVB197_SET_PIXEL_SHADER_INTERLOCK_CONTROL_TILE_COALESCER_MODE_CONFLICT_DETECT_SAMPLE;
   const uint32_t pixel = NVB197_SET_PIXEL_SHADER_INTERLOCK_CONTROL_TILE_COALESCER_MODE_CONFLICT_DETECT_PIXEL;

   /* metal2vulkan's multisampled fetch: the storage form in the pixel
    * interlock, read at SampleId, the pipeline's sample shading off
    */
   const struct interlocked fetch = fragment_push(dev, true, false);
   check(fetch.anti_alias == nvk_mme_anti_alias_min_sample_shading(1),
         "a SampleId read shades one pass per sample");
   check(CONFLICT_MODE(fetch.control) == sample,
         "and so detects conflicts per sample, not per pixel");
   check(CONFLICT_MODE(fetch.stage_control) == sample,
         "the raw stage takes the same detection");

   const struct interlocked shaded = fragment_push(dev, false, true);
   check(CONFLICT_MODE(shaded.control) == sample &&
            CONFLICT_MODE(shaded.stage_control) == sample,
         "the pipeline's sample shading detects conflicts per sample");

   const struct interlocked per_pixel = fragment_push(dev, false, false);
   check(per_pixel.anti_alias == nvk_mme_anti_alias_min_sample_shading(0),
         "a shader without sample shading shades one pass");
   check(CONFLICT_MODE(per_pixel.control) == pixel &&
            CONFLICT_MODE(per_pixel.stage_control) == pixel,
         "and keeps pixel conflict detection");

   free(dev);
   free(pdev);
   if (failures == 0)
      printf("interlock: ok\n");
   return failures ? 1 : 0;
}
