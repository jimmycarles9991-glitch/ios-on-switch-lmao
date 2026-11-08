// SPDX-License-Identifier: MIT
// Maxwell's tiled cache (the binning rasterizer) on the raw path's context, when the kernel gave every GR context the tile
// coalescer's table (TegraGM20BInfoTiledCache, drivers/TegraGM20B/TegraGM20BTiledCache.h). NVIDIA's class headers name
// only 0x1134 (SET_MUTABLE_METHOD_CONTROL, deko3d's TiledCacheUnkFeatureEnable); the other offsets and NVN's Tegra X1
// values are deko3d's (source/maxwell/engine_3d.def, gpu_3d_base.cpp Queue::setup3DEngine, MIT).
//
// A WAIT_FOR_IDLE completes binned work (deko3d's DkBarrier_Primitives is a WFI alone), and every raw pass starts with
// one, so render-target changes need nothing more. The flush that ends each command buffer leaves nothing binned for the
// kernel's fence.
#pragma once
#include <os/log.h>
#include <atomic>
#include <cstdint>
#include "TegraGM20BABI.h"
#include "raw_push.h"

namespace raw {

enum : uint32_t {
    SET_TILED_CACHE_ENABLE = 0x0f60,
    SET_TILED_CACHE_TILE_SIZE = 0x0f64,
    SET_TILED_CACHE_CONFIG_0 = 0x0f68,
    SET_TILED_CACHE_CONFIG_1 = 0x0f6c,
    SET_TILED_CACHE_CONFIG_3 = 0x0f70,
    TILED_CACHE_BARRIER = 0x0f7c,
    TILED_CACHE_FLUSH = 0x0f80,
    SET_TILED_CACHE_CONFIG_2 = 0x1108,
    SET_MUTABLE_METHOD_CONTROL = 0x1134,
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
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        os_log(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw"),
               "raw: tiled cache on, enabled at context init");
    }
}

inline uint32_t *tiledCacheFlush(uint32_t *at)
{
    Push p{at, at + kTiledCacheFlushWords};
    p.m(TILED_CACHE_FLUSH, 0);
    return p.at;
}

}  // namespace raw
