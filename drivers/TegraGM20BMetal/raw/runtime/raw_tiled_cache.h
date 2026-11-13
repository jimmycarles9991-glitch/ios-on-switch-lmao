// SPDX-License-Identifier: MIT
// Maxwell's tiled cache (the binning rasterizer) on the raw path's context, when the kernel gave every GR context the tile
// coalescer's table (TegraGM20BInfoTiledCache, drivers/TegraGM20B/TegraGM20BTiledCache.h). NVIDIA's class headers name
// only SET_MUTABLE_METHOD_CONTROL (deko3d's TiledCacheUnkFeatureEnable); the other methods (kits/SwitchGM20B's
// SwitchGM20BTegra.h) and NVN's Tegra X1 values are deko3d's (source/maxwell/engine_3d.def, gpu_3d_base.cpp
// Queue::setup3DEngine, MIT).
//
// Every pass that drew and every barrier inside a pass flush the cache before the WAIT_FOR_IDLE that follows them, so a later
// pass, the copy engine, the display's copy or the CPU reads what the draws wrote: whether a WAIT_FOR_IDLE alone drains the
// tile coalescer is not documented. The flush that ends each command buffer leaves nothing binned for the kernel's fence.
#pragma once
#include <os/log.h>
#include <atomic>
#include <cstdint>
#include "../../../../kits/SwitchGM20B/SwitchGM20B3D.h"
#include "TegraGM20BABI.h"
#include "raw_push.h"
#include "raw_stats.h"

namespace raw {

enum : uint32_t {
    SET_TILED_CACHE_ENABLE = SWITCH_NVB197_SET_TILED_CACHE_ENABLE,
    SET_TILED_CACHE_TILE_SIZE = SWITCH_NVB197_SET_TILED_CACHE_TILE_SIZE,
    SET_TILED_CACHE_CONFIG_0 = SWITCH_NVB197_SET_TILED_CACHE_CONFIG_0,
    SET_TILED_CACHE_CONFIG_1 = SWITCH_NVB197_SET_TILED_CACHE_CONFIG_1,
    SET_TILED_CACHE_CONFIG_3 = SWITCH_NVB197_SET_TILED_CACHE_CONFIG_3,
    TILED_CACHE_BARRIER = SWITCH_NVB197_TILED_CACHE_BARRIER,
    TILED_CACHE_FLUSH = SWITCH_NVB197_TILED_CACHE_FLUSH,
    SET_TILED_CACHE_CONFIG_2 = SWITCH_NVB197_SET_TILED_CACHE_CONFIG_2,
    SET_MUTABLE_METHOD_CONTROL = NVB197_SET_MUTABLE_METHOD_CONTROL,
};

constexpr uint32_t kTiledCacheFlushWords = 1;

inline bool tiledCache(const TegraGM20BAbiInfo &info)
{
    return (info.flags & TegraGM20BInfoTiledCache) != 0;
}

inline void tiledCacheInit(Push &p, const TegraGM20BAbiInfo &info)
{
    if (!tiledCache(info))
        return;
    p.m(SET_TILED_CACHE_TILE_SIZE, 128 | 128 << 16);
    p.m(SET_TILED_CACHE_CONFIG_0, 0x1109);
    p.m(SET_TILED_CACHE_CONFIG_1, 0x08080202);
    p.m(SET_TILED_CACHE_CONFIG_2, 0x1f);
    p.m(SET_TILED_CACHE_CONFIG_3, 0x00080001);
    p.m(SET_MUTABLE_METHOD_CONTROL, 0);
    p.m(SET_TILED_CACHE_ENABLE, 1);
#if SWITCH_RAW_DIAGNOSTICS
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        os_log(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw"),
               "raw: tiled cache on, enabled at context init");
    }
#endif
}

inline void tiledCacheDrain(Push &p, const TegraGM20BAbiInfo &info)
{
    if (tiledCache(info))
        p.m(TILED_CACHE_FLUSH, 0);
}

inline uint32_t *tiledCacheFlush(uint32_t *at)
{
    Push p{at, at + kTiledCacheFlushWords};
    p.m(TILED_CACHE_FLUSH, 0);
    return p.at;
}

}  // namespace raw
