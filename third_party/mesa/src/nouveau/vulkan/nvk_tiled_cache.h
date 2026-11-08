/*
 * Copyright © 2026 Switch
 * SPDX-License-Identifier: MIT
 *
 * Maxwell B's tiled cache: the binning rasterizer, which draws the primitives
 * it has buffered one screen tile at a time so a tile's blending stays in L2.
 * NVIDIA's class headers leave its methods out; the offsets and NVN's Tegra X1
 * configuration are deko3d's (source/maxwell/engine_3d.def, gpu_3d_base.cpp).
 * It is used only when the kernel put the tile coalescer's bundle table in
 * every GR context (nvkmd_info::has_tiled_cache); otherwise none of this is
 * emitted.
 */
#ifndef NVK_TILED_CACHE_H
#define NVK_TILED_CACHE_H 1

#include "nvk_cmd_buffer.h"
#include "nvk_device.h"
#include "nvk_physical_device.h"
#include "nvkmd/nvkmd.h"

#include "clb197.h"

#define NVK_TILED_CACHE_ENABLE    0x0f60
#define NVK_TILED_CACHE_TILE_SIZE 0x0f64
#define NVK_TILED_CACHE_CONFIG_0  0x0f68
#define NVK_TILED_CACHE_CONFIG_1  0x0f6c
#define NVK_TILED_CACHE_CONFIG_3  0x0f70
#define NVK_TILED_CACHE_FLUSH     0x0f80
#define NVK_TILED_CACHE_CONFIG_2  0x1108

static inline bool
nvk_has_tiled_cache(const struct nvk_physical_device *pdev)
{
   return pdev->nvkmd->kmd_info.has_tiled_cache;
}

static inline void
nvk_push_tiled_cache_mthd(struct nv_push *p, uint32_t mthd, uint32_t val)
{
   if (val < 0x2000) {
      __push_immd(p, SUBC_NV9097, mthd, val);
   } else {
      __push_mthd(p, SUBC_NV9097, mthd);
      nv_push_val(p, mthd, val);
   }
}

/* NVN's configuration in deko3d's order, then the enable */
static inline void
nvk_push_tiled_cache_init(struct nv_push *p,
                          const struct nvk_physical_device *pdev)
{
   if (!nvk_has_tiled_cache(pdev))
      return;

   nvk_push_tiled_cache_mthd(p, NVK_TILED_CACHE_TILE_SIZE, 128 | 128 << 16);
   nvk_push_tiled_cache_mthd(p, NVK_TILED_CACHE_CONFIG_0, 0x00001109);
   nvk_push_tiled_cache_mthd(p, NVK_TILED_CACHE_CONFIG_1, 0x08080202);
   nvk_push_tiled_cache_mthd(p, NVK_TILED_CACHE_CONFIG_2, 0x0000001f);
   nvk_push_tiled_cache_mthd(p, NVK_TILED_CACHE_CONFIG_3, 0x00080001);
   nvk_push_tiled_cache_mthd(p, NVB197_SET_MUTABLE_METHOD_CONTROL, 0);
   nvk_push_tiled_cache_mthd(p, NVK_TILED_CACHE_ENABLE, 1);
}

/* After a release down the 3D pipe (SET_REPORT_SEMAPHORE), and at the end of
 * a command buffer: the cache can hold a release back with the primitives it
 * has binned until it next flushes, so a wait on it, even a semaphore acquire
 * later in the same submission, could stall. NVN flushes after each of its
 * releases (deko3d's dk_queue.cpp signalFence).
 */
static inline void
nvk_cmd_flush_tiled_cache(struct nvk_cmd_buffer *cmd)
{
   const struct nvk_physical_device *pdev =
      nvk_device_physical(nvk_cmd_buffer_device(cmd));

   if (!nvk_has_tiled_cache(pdev) ||
       !(nvk_cmd_buffer_queue_flags(cmd) & VK_QUEUE_GRAPHICS_BIT))
      return;

   struct nv_push *p = nvk_cmd_buffer_push(cmd, 1);
   __push_immd(p, SUBC_NV9097, NVK_TILED_CACHE_FLUSH, 0);
}

#endif /* NVK_TILED_CACHE_H */
