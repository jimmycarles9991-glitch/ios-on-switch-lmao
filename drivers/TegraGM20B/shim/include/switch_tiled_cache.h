/* SPDX-License-Identifier: MIT */
/* Maxwell's tiled cache, the kernel's part (TegraGM20BTiledCache.h). switch_tiled_cache_golden: nvkm's gm20b golden
 * context gets the tile coalescer's table. switch_tiled_cache_on: it did (no switch-no-tiled-cache boot-arg), so GetInfo
 * sets TegraGM20BInfoTiledCache. */
#ifndef SWITCH_TILED_CACHE_H
#define SWITCH_TILED_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

struct nvkm_device;

void switch_tiled_cache_golden(struct nvkm_device *device);
int switch_tiled_cache_on(void);

#ifdef __cplusplus
}
#endif
#endif
