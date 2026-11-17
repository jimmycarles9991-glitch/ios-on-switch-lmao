// SPDX-License-Identifier: MIT
// raw/fx's CPU copies of a shared multisampled texture (fx::writeSamples, readSamples): Metal's rows hold each pixel's
// samples in turn (an M4 Max's getBytes and replaceRegion, m2vf-targets2 probe/ms/mscpu.mm), and each sample lands in the
// cell of NIL's sample grid its index names (nil/image.rs SampleLayout::sa_offset: 2X1_D3D, 2X2, 4X2_D3D), at the
// TegraColor GOB address TegraGM20BLayout.h gives that grid texel.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "../raw/fx/fx_image.h"
#include "TegraGM20BLayout.h"

namespace {

struct Cell {
    uint32_t x, y;
};

// nil/image.rs sa_offset
Cell nilOffset(uint32_t samples, uint32_t s)
{
    static const Cell k4x2D3d[8] = {{2, 0}, {1, 1}, {3, 1}, {1, 0}, {0, 1}, {0, 0}, {2, 1}, {3, 0}};
    return samples == 2 ? Cell{1 - s, 0} : samples == 4 ? Cell{s & 1, s >> 1} : k4x2D3d[s];
}

uint32_t texel(uint32_t x, uint32_t y, uint32_t s)
{
    return 0x80000000u | y << 16 | x << 8 | s;
}

void landsInItsCell(uint32_t samples)
{
    const uint32_t width = 13, height = 9, bpp = 4;
    const uint32_t cellW = samples == 8 ? 4 : 2, cellH = samples / cellW;
    raw::fx::MipChain c;
    ASSERT_TRUE(raw::fx::msLayout(width, height, samples, bpp, c));
    std::vector<uint8_t> memory(c.bytes);
    std::vector<uint32_t> metal(size_t(width) * height * samples);
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++)
            for (uint32_t s = 0; s < samples; s++)
                metal[(size_t(y) * width + x) * samples + s] = texel(x, y, s);
    const uint32_t at[2] = {0, 0}, size[2] = {width, height};
    raw::fx::writeSamples(c, memory.data(), samples, at, size, reinterpret_cast<const uint8_t *>(metal.data()),
                          size_t(width) * samples * bpp);
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++)
            for (uint32_t s = 0; s < samples; s++) {
                const Cell cell = nilOffset(samples, s);
                const uint64_t offset = tgm_layout_offset_of(c.level[0].rowBytes, c.level[0].yLog2,
                                                             (x * cellW + cell.x) * bpp, y * cellH + cell.y);
                uint32_t found;
                memcpy(&found, &memory[offset], 4);
                ASSERT_EQ(found, texel(x, y, s)) << "pixel " << x << "," << y << " sample " << s;
            }
}

void readsMetalsRows(uint32_t samples)
{
    const uint32_t width = 13, height = 9, bpp = 4;
    raw::fx::MipChain c;
    ASSERT_TRUE(raw::fx::msLayout(width, height, samples, bpp, c));
    std::vector<uint8_t> memory(c.bytes);
    std::vector<uint32_t> metal(size_t(width) * height * samples);
    for (uint32_t i = 0; i < metal.size(); i++)
        metal[i] = texel(uint32_t(i / samples) % width, uint32_t(i / samples) / width, i % samples);
    const uint32_t whole[2] = {0, 0}, all[2] = {width, height};
    raw::fx::writeSamples(c, memory.data(), samples, whole, all, reinterpret_cast<const uint8_t *>(metal.data()),
                          size_t(width) * samples * bpp);
    const uint32_t at[2] = {3, 2}, size[2] = {7, 5};
    std::vector<uint32_t> part(size_t(size[0]) * size[1] * samples);
    raw::fx::readSamples(c, memory.data(), samples, at, size, reinterpret_cast<uint8_t *>(part.data()),
                         size_t(size[0]) * samples * bpp);
    for (uint32_t y = 0; y < size[1]; y++)
        for (uint32_t x = 0; x < size[0]; x++)
            for (uint32_t s = 0; s < samples; s++)
                ASSERT_EQ(part[(size_t(y) * size[0] + x) * samples + s], texel(at[0] + x, at[1] + y, s));
}

TEST(MultisampleCpu, EachSampleLandsInItsGridCell)
{
    for (const uint32_t samples : {2u, 4u, 8u})
        landsInItsCell(samples);
}

TEST(MultisampleCpu, AReadOfARectangleIsMetalsRows)
{
    for (const uint32_t samples : {2u, 4u, 8u})
        readsMetalsRows(samples);
}

}  // namespace
