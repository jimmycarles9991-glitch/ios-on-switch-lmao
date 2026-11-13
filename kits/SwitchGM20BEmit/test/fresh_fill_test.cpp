// SPDX-License-Identifier: MIT
// raw::BlitEncoder::fillTexels's words: the remap constants, then the kit's rectangle copy of a block-linear level onto itself
// with every destination component a constant (raw_fresh.h copyFillOf), as the copy engine takes them.
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/raw/runtime/raw_fresh.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_kit.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BEmitCopy.h"
#include "push_words.h"

namespace {

using switch_gm20b_test::Method;
using switch_gm20b_test::Methods;

std::vector<Method> fill(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch, uint8_t blockHeight)
{
    raw::FreshTexel texel;
    raw::CopyFill f;
    EXPECT_TRUE(raw::freshPatternTexel(format, texel));
    EXPECT_TRUE(raw::copyFillOf(texel.bytes, texel.size, f));
    std::vector<uint32_t> words(256);
    raw::Push p{words.data(), words.data() + words.size()};
    p.m(NVB0B5_SET_REMAP_CONST_A, f.constA, raw::SUBC_COPY);
    p.m(NVB0B5_SET_REMAP_CONST_B, f.constB, raw::SUBC_COPY);
    raw::CopySurface side;
    side.va = 0x40000000;
    side.pitch = pitch;
    side.blockLinear = true;
    side.height = height;
    side.depth = 1;
    side.blockHeight = blockHeight;
    raw::emitCopy(p, side, side, width, height, f.remap, false);
    words.resize(size_t(p.at - words.data()));
    return Methods(words);
}

uint32_t value(const std::vector<Method> &methods, uint32_t method)
{
    for (const Method &m : methods)
        if (m.subchannel == raw::SUBC_COPY && m.method == method)
            return m.value;
    ADD_FAILURE() << "no method 0x" << std::hex << method;
    return 0;
}

}  // namespace

TEST(FreshFill, AHalfFloatLevelTakesBothConstantsInEightByteTexels)
{
    const auto m = fill(115, 832, 1408, 832 * 8, 4);
    EXPECT_EQ(value(m, NVB0B5_SET_REMAP_CONST_A), 0x0000ffffu);
    EXPECT_EQ(value(m, NVB0B5_SET_REMAP_CONST_B), 0xffffffffu);
    const uint32_t remap = value(m, NVB0B5_SET_REMAP_COMPONENTS);
    EXPECT_EQ(remap & 7, NVB0B5_SET_REMAP_COMPONENTS_DST_X_CONST_A);
    EXPECT_EQ(remap >> 4 & 7, NVB0B5_SET_REMAP_COMPONENTS_DST_Y_CONST_B);
    EXPECT_EQ(remap >> 16 & 3, NVB0B5_SET_REMAP_COMPONENTS_COMPONENT_SIZE_FOUR);
    EXPECT_EQ(remap >> 24 & 3, NVB0B5_SET_REMAP_COMPONENTS_NUM_DST_COMPONENTS_TWO);
    EXPECT_EQ(value(m, NVB0B5_LINE_LENGTH_IN), 832u);
    EXPECT_EQ(value(m, NVB0B5_LINE_COUNT), 1408u);
    EXPECT_EQ(value(m, NVB0B5_SET_DST_WIDTH), 832u);
    EXPECT_EQ(value(m, NVB0B5_SET_DST_HEIGHT), 1408u);
    const uint32_t launch = value(m, NVB0B5_LAUNCH_DMA);
    EXPECT_TRUE(launch >> 10 & 1);
    EXPECT_EQ(launch >> 8 & 1, NVB0B5_LAUNCH_DMA_DST_MEMORY_LAYOUT_BLOCKLINEAR);
    EXPECT_EQ(value(m, NVB0B5_SET_DST_BLOCK_SIZE) >> 4 & 0xf, 4u);
}

TEST(FreshFill, AnEightBitColourLevelTakesOneWord)
{
    const auto m = fill(80, 1280, 108, 1280 * 4, 4);
    EXPECT_EQ(value(m, NVB0B5_SET_REMAP_CONST_A), 0xffff00ffu);
    const uint32_t remap = value(m, NVB0B5_SET_REMAP_COMPONENTS);
    EXPECT_EQ(remap & 7, NVB0B5_SET_REMAP_COMPONENTS_DST_X_CONST_A);
    EXPECT_EQ(remap >> 24 & 3, NVB0B5_SET_REMAP_COMPONENTS_NUM_DST_COMPONENTS_ONE);
    EXPECT_EQ(value(m, NVB0B5_LINE_LENGTH_IN), 1280u);
    EXPECT_EQ(value(m, NVB0B5_SET_DST_WIDTH), 1280u);
}

TEST(FreshFill, AFloatQuadLevelTakesFourComponents)
{
    const auto m = fill(125, 64, 64, 64 * 16, 3);
    const uint32_t remap = value(m, NVB0B5_SET_REMAP_COMPONENTS);
    EXPECT_EQ(remap >> 24 & 3, NVB0B5_SET_REMAP_COMPONENTS_NUM_DST_COMPONENTS_FOUR);
    EXPECT_EQ(remap >> 4 & 7, NVB0B5_SET_REMAP_COMPONENTS_DST_Y_CONST_B);
    EXPECT_EQ(remap >> 8 & 7, NVB0B5_SET_REMAP_COMPONENTS_DST_Z_CONST_A);
    EXPECT_EQ(value(m, NVB0B5_SET_REMAP_CONST_B), 0u);
    EXPECT_EQ(value(m, NVB0B5_SET_DST_WIDTH), 64u);
}
