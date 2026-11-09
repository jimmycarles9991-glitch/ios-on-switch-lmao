// SPDX-License-Identifier: MIT
// raw/runtime/raw_tiled_cache.h on the host: with the kernel's TegraGM20BInfoTiledCache the raw path's context gets
// deko3d's tiled-cache configuration (gpu_3d_base.cpp:226-231, NVN's on the Tegra X1) and then the enable, each pass's
// draws and each command buffer's tail end in one TiledCacheFlush; without the bit nothing is emitted.
#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

#include "../raw/runtime/raw_tiled_cache.h"

namespace {

using Methods = std::vector<std::pair<uint32_t, uint32_t>>;

// Subchannel 0's (method, value) pairs, as the host reads NV906F headers: IMMD (op 4) and INC (op 1) only
Methods decode(const uint32_t *at, const uint32_t *end)
{
    Methods m;
    while (at < end) {
        const uint32_t h = *at++, op = h >> 29, count = (h >> 16) & 0x1fff, mthd = (h & 0xfff) << 2;
        EXPECT_EQ((h >> 13) & 7, 0u);
        if (op == 4) {
            m.push_back({mthd, count});
            continue;
        }
        EXPECT_EQ(op, 1u);
        for (uint32_t i = 0; i < count; i++)
            m.push_back({mthd + 4 * i, *at++});
    }
    return m;
}

TegraGM20BAbiInfo info(uint32_t flags)
{
    TegraGM20BAbiInfo i{};
    i.flags = flags;
    return i;
}

TEST(RawTiledCache, InitIsDeko3dsConfigurationThenTheEnable)
{
    uint32_t words[64];
    raw::Push p{words, words + 64};
    raw::tiledCacheInit(p, info(TegraGM20BInfoHostImport | TegraGM20BInfoTiledCache));
    const Methods want = {{0x0f64, 0x00800080}, {0x0f68, 0x00001109}, {0x0f6c, 0x08080202}, {0x1108, 0x0000001f},
                          {0x0f70, 0x00080001}, {0x1134, 0},          {0x0f60, 1}};
    EXPECT_EQ(decode(words, p.at), want);
}

TEST(RawTiledCache, NothingWithoutTheKernelsTable)
{
    uint32_t words[64];
    raw::Push p{words, words + 64};
    raw::tiledCacheInit(p, info(TegraGM20BInfoHostImport | TegraGM20BInfoOrder));
    EXPECT_EQ(p.at, words);
    EXPECT_FALSE(raw::tiledCache(info(0)));
}

TEST(RawTiledCache, PassEndDrainsTheCoalescer)
{
    uint32_t words[4];
    raw::Push p{words, words + 4};
    raw::tiledCacheDrain(p, info(TegraGM20BInfoHostImport | TegraGM20BInfoTiledCache));
    EXPECT_EQ(decode(words, p.at), (Methods{{0x0f80, 0}}));
    raw::Push off{words, words + 4};
    raw::tiledCacheDrain(off, info(TegraGM20BInfoHostImport));
    EXPECT_EQ(off.at, words);
}

TEST(RawTiledCache, TailIsOneFlush)
{
    uint32_t word = 0;
    EXPECT_EQ(raw::tiledCacheFlush(&word), &word + 1);
    EXPECT_EQ(decode(&word, &word + 1), (Methods{{0x0f80, 0}}));
}

}  // namespace
