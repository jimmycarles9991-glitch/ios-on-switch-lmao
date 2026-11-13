// SPDX-License-Identifier: MIT
// The 2D solid fill against the methods of x1-units' reference fill (raw_probe_ops.h, defb0dac0) and xf86-video-nouveau's
// EXA fill: the destination's own format and one packed word, SRCCOPY, clip at the surface.
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "kits/SwitchGM20B/SwitchGM20B2D.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BEmit2D.h"
#include "push_words.h"

namespace {

using switch_gm20b_test::Method;
using switch_gm20b_test::Methods;

constexpr uint32_t k2D = SUBC_NV902D;

std::vector<uint32_t> fill(const switch_gm20b_2d_surface &s)
{
    std::vector<uint32_t> words(64);
    nv_push p;
    nv_push_init(&p, words.data(), words.size(), SUBC_MASK_ALL);
    switch_gm20b_2d_fill(&p, &s, 0xff336699u, 8, 4, 40, 20);
    words.resize(nv_push_dw_count(&p));
    EXPECT_LE(words.size(), SWITCH_GM20B_2D_FILL_WORDS);
    return words;
}

std::vector<Method> fillTail(const switch_gm20b_2d_surface &s)
{
    return {
        {k2D, NV902D_SET_OPERATION, NV902D_SET_OPERATION_V_SRCCOPY},
        {k2D, NV902D_SET_COLOR_KEY_ENABLE, 0},
        {k2D, NV902D_SET_RENDER_ENABLE_C, NV902D_SET_RENDER_ENABLE_C_MODE_TRUE},
        {k2D, NV902D_SET_CLIP_X0, 0},
        {k2D, NV902D_SET_CLIP_Y0, 0},
        {k2D, NV902D_SET_CLIP_WIDTH, s.width},
        {k2D, NV902D_SET_CLIP_HEIGHT, s.height},
        {k2D, NV902D_SET_CLIP_ENABLE, NV902D_SET_CLIP_ENABLE_V_TRUE},
        {k2D, NV902D_RENDER_SOLID_PRIM_MODE, NV902D_RENDER_SOLID_PRIM_MODE_V_RECTS},
        {k2D, NV902D_SET_RENDER_SOLID_PRIM_COLOR_FORMAT, s.format},
        {k2D, NV902D_SET_RENDER_SOLID_PRIM_COLOR, 0xff336699u},
        {k2D, NV902D_RENDER_SOLID_PRIM_POINT_SET_X(0), 8},
        {k2D, NV902D_RENDER_SOLID_PRIM_POINT_Y(0), 4},
        {k2D, NV902D_RENDER_SOLID_PRIM_POINT_SET_X(1), 40},
        {k2D, NV902D_RENDER_SOLID_PRIM_POINT_Y(1), 20},
        {k2D, NV902D_SET_CLIP_ENABLE, NV902D_SET_CLIP_ENABLE_V_FALSE},
    };
}

TEST(Emit2D, ABlockLinearFillIsTheReferenceFill)
{
    switch_gm20b_2d_surface s = {};
    s.address = 0x0123456000ull;
    s.format = NV902D_SET_DST_FORMAT_V_A8R8G8B8;
    s.block_height_log2 = 4;
    s.width = 1280;
    s.height = 720;
    std::vector<Method> expected = {
        {k2D, NV902D_SET_DST_COLOR_RENDER_TO_ZETA_SURFACE, 0},
        {k2D, NV902D_SET_COMPRESSION, 0},
        {k2D, NV902D_SET_DST_FORMAT, NV902D_SET_DST_FORMAT_V_A8R8G8B8},
        {k2D, NV902D_SET_DST_MEMORY_LAYOUT, NV902D_SET_DST_MEMORY_LAYOUT_V_BLOCKLINEAR},
        {k2D, NV902D_SET_DST_BLOCK_SIZE, 4u << 4},
        {k2D, NV902D_SET_DST_DEPTH, 1},
        {k2D, NV902D_SET_DST_LAYER, 0},
        {k2D, NV902D_SET_DST_WIDTH, 1280},
        {k2D, NV902D_SET_DST_HEIGHT, 720},
        {k2D, NV902D_SET_DST_OFFSET_UPPER, 0x01},
        {k2D, NV902D_SET_DST_OFFSET_LOWER, 0x23456000},
    };
    const auto tail = fillTail(s);
    expected.insert(expected.end(), tail.begin(), tail.end());
    EXPECT_EQ(Methods(fill(s)), expected);
}

TEST(Emit2D, APitchFillNamesItsPitch)
{
    switch_gm20b_2d_surface s = {};
    s.address = 0x0100000000ull;
    s.format = NV902D_SET_DST_FORMAT_V_A8B8G8R8;
    s.pitch = true;
    s.pitch_bytes = 5120;
    s.width = 1280;
    s.height = 720;
    std::vector<Method> expected = {
        {k2D, NV902D_SET_DST_COLOR_RENDER_TO_ZETA_SURFACE, 0},
        {k2D, NV902D_SET_COMPRESSION, 0},
        {k2D, NV902D_SET_DST_FORMAT, NV902D_SET_DST_FORMAT_V_A8B8G8R8},
        {k2D, NV902D_SET_DST_MEMORY_LAYOUT, NV902D_SET_DST_MEMORY_LAYOUT_V_PITCH},
        {k2D, NV902D_SET_DST_PITCH, 5120},
        {k2D, NV902D_SET_DST_WIDTH, 1280},
        {k2D, NV902D_SET_DST_HEIGHT, 720},
        {k2D, NV902D_SET_DST_OFFSET_UPPER, 0x01},
        {k2D, NV902D_SET_DST_OFFSET_LOWER, 0},
    };
    const auto tail = fillTail(s);
    expected.insert(expected.end(), tail.begin(), tail.end());
    EXPECT_EQ(Methods(fill(s)), expected);
}

TEST(Emit2D, CompressionAndZetaAreTheSurfaces)
{
    switch_gm20b_2d_surface s = {};
    s.format = NV902D_SET_DST_FORMAT_V_A8R8G8B8;
    s.block_height_log2 = 4;
    s.width = 64;
    s.height = 64;
    s.compressed = true;
    s.zeta = true;
    const auto m = Methods(fill(s));
    ASSERT_GE(m.size(), 2u);
    EXPECT_EQ(m[0], (Method{k2D, NV902D_SET_DST_COLOR_RENDER_TO_ZETA_SURFACE, 1}));
    EXPECT_EQ(m[1], (Method{k2D, NV902D_SET_COMPRESSION, 1}));
}

TEST(Emit2D, AnSrgbSurfaceIsFilledThroughItsUnormView)
{
    const std::pair<uint32_t, uint32_t> twins[] = {
        {NV902D_SET_DST_FORMAT_V_A8RL8GL8BL8, NV902D_SET_DST_FORMAT_V_A8R8G8B8},
        {NV902D_SET_DST_FORMAT_V_A8BL8GL8RL8, NV902D_SET_DST_FORMAT_V_A8B8G8R8},
        {NV902D_SET_DST_FORMAT_V_X8RL8GL8BL8, NV902D_SET_DST_FORMAT_V_X8R8G8B8},
        {NV902D_SET_DST_FORMAT_V_X8BL8GL8RL8, NV902D_SET_DST_FORMAT_V_X8B8G8R8},
        {NV902D_SET_DST_FORMAT_V_RF16_GF16_BF16_AF16, NV902D_SET_DST_FORMAT_V_RF16_GF16_BF16_AF16},
    };
    for (const auto &[srgb, unorm] : twins) {
        switch_gm20b_2d_surface s = {};
        s.format = srgb;
        s.block_height_log2 = 4;
        s.width = 64;
        s.height = 64;
        uint32_t dst = 0, solid = 0;
        for (const Method &m : Methods(fill(s))) {
            if (m.method == NV902D_SET_DST_FORMAT)
                dst = m.value;
            if (m.method == NV902D_SET_RENDER_SOLID_PRIM_COLOR_FORMAT)
                solid = m.value;
        }
        EXPECT_EQ(switch_gm20b_2d_fill_format(srgb), unorm);
        EXPECT_EQ(dst, unorm) << srgb;
        EXPECT_EQ(solid, unorm) << srgb;
    }
}

TEST(Emit2D, AScaledBlitIsTheProbesScaleOp)
{
    switch_gm20b_2d_surface src = {}, dst = {};
    src.address = 0x0100000000ull;
    src.format = NV902D_SET_SRC_FORMAT_V_RF16_GF16_BF16_AF16;
    src.block_height_log2 = 4;
    src.width = 512;
    src.height = 256;
    dst = src;
    dst.address = 0x0200000000ull;
    dst.width = 256;
    dst.height = 128;
    switch_gm20b_2d_scale_op s = {0, 0, 256, 128, 2ull << 32, 2ull << 32, 1ull << 32, 1ull << 32, true, true, 0};
    std::vector<uint32_t> words(64);
    nv_push p;
    nv_push_init(&p, words.data(), words.size(), SUBC_MASK_ALL);
    switch_gm20b_2d_scale(&p, &src, &dst, &s);
    words.resize(nv_push_dw_count(&p));
    EXPECT_LE(words.size(), SWITCH_GM20B_2D_SCALE_WORDS);
    const std::vector<Method> expected = {
        {k2D, NV902D_SET_DST_COLOR_RENDER_TO_ZETA_SURFACE, 0},
        {k2D, NV902D_SET_COMPRESSION, 0},
        {k2D, NV902D_SET_DST_FORMAT, NV902D_SET_SRC_FORMAT_V_RF16_GF16_BF16_AF16},
        {k2D, NV902D_SET_DST_MEMORY_LAYOUT, NV902D_SET_DST_MEMORY_LAYOUT_V_BLOCKLINEAR},
        {k2D, NV902D_SET_DST_BLOCK_SIZE, 4u << 4},
        {k2D, NV902D_SET_DST_DEPTH, 1},
        {k2D, NV902D_SET_DST_LAYER, 0},
        {k2D, NV902D_SET_DST_WIDTH, 256},
        {k2D, NV902D_SET_DST_HEIGHT, 128},
        {k2D, NV902D_SET_DST_OFFSET_UPPER, 0x02},
        {k2D, NV902D_SET_DST_OFFSET_LOWER, 0},
        {k2D, NV902D_SET_SRC_FORMAT, NV902D_SET_SRC_FORMAT_V_RF16_GF16_BF16_AF16},
        {k2D, NV902D_SET_SRC_MEMORY_LAYOUT, NV902D_SET_SRC_MEMORY_LAYOUT_V_BLOCKLINEAR},
        {k2D, NV902D_SET_SRC_BLOCK_SIZE, 4u << 4},
        {k2D, NV902D_SET_SRC_DEPTH, 1},
        {k2D, NV902D_SET_SRC_WIDTH, 512},
        {k2D, NV902D_SET_SRC_HEIGHT, 256},
        {k2D, NV902D_SET_SRC_OFFSET_UPPER, 0x01},
        {k2D, NV902D_SET_SRC_OFFSET_LOWER, 0},
        {k2D, NV902D_TWOD_INVALIDATE_TEXTURE_DATA_CACHE, NV902D_TWOD_INVALIDATE_TEXTURE_DATA_CACHE_V_L1_ONLY},
        {k2D, NV902D_SET_PIXELS_FROM_MEMORY_SAMPLE_MODE, 0x11},
        {k2D, NV902D_SET_PIXELS_FROM_MEMORY_DST_X0, 0},
        {k2D, NV902D_SET_PIXELS_FROM_MEMORY_DST_Y0, 0},
        {k2D, NV902D_SET_PIXELS_FROM_MEMORY_DST_WIDTH, 256},
        {k2D, NV902D_SET_PIXELS_FROM_MEMORY_DST_HEIGHT, 128},
        {k2D, NV902D_SET_PIXELS_FROM_MEMORY_DU_DX_FRAC, 0},
        {k2D, NV902D_SET_PIXELS_FROM_MEMORY_DU_DX_INT, 2},
        {k2D, NV902D_SET_PIXELS_FROM_MEMORY_DV_DY_FRAC, 0},
        {k2D, NV902D_SET_PIXELS_FROM_MEMORY_DV_DY_INT, 2},
        {k2D, NV902D_SET_PIXELS_FROM_MEMORY_SRC_X0_FRAC, 0},
        {k2D, NV902D_SET_PIXELS_FROM_MEMORY_SRC_X0_INT, 1},
        {k2D, NV902D_SET_PIXELS_FROM_MEMORY_SRC_Y0_FRAC, 0},
        {k2D, NV902D_PIXELS_FROM_MEMORY_SRC_Y0_INT, 1},
    };
    EXPECT_EQ(Methods(words), expected);
}

std::vector<Method> ropOn(uint32_t rop)
{
    return {
        {k2D, NV902D_SET_OPERATION, NV902D_SET_OPERATION_V_ROP},
        {k2D, NV902D_SET_ROP, rop},
        {k2D, NV902D_SET_MONOCHROME_PATTERN_COLOR_FORMAT, NV902D_SET_MONOCHROME_PATTERN_COLOR_FORMAT_V_A8R8G8B8},
        {k2D, NV902D_SET_MONOCHROME_PATTERN_FORMAT, NV902D_SET_MONOCHROME_PATTERN_FORMAT_V_LE_M1},
        {k2D, NV902D_SET_MONOCHROME_PATTERN_COLOR0, ~0u},
        {k2D, NV902D_SET_MONOCHROME_PATTERN_COLOR1, ~0u},
        {k2D, NV902D_SET_MONOCHROME_PATTERN0, ~0u},
        {k2D, NV902D_SET_MONOCHROME_PATTERN1, ~0u},
    };
}

TEST(Emit2D, AnInvertIsTheDdxsGxInvertFillThenSrccopyAgain)
{
    switch_gm20b_2d_surface s = {};
    s.address = 0x0123456000ull;
    s.format = NV902D_SET_DST_FORMAT_V_A8B8G8R8;
    s.block_height_log2 = 4;
    s.width = 1440;
    s.height = 2560;
    std::vector<uint32_t> words(64);
    nv_push p;
    nv_push_init(&p, words.data(), words.size(), SUBC_MASK_ALL);
    switch_gm20b_2d_invert(&p, &s, 0, 0, 1440, 2560);
    words.resize(nv_push_dw_count(&p));
    EXPECT_LE(words.size(), SWITCH_GM20B_2D_INVERT_WORDS);
    const std::vector<Method> m = Methods(words);
    ASSERT_EQ(m.size(), 11u + 8u + 15u + 1u);
    EXPECT_EQ(std::vector<Method>(m.begin() + 11, m.begin() + 19), ropOn(SWITCH_GM20B_2D_ROP_DSTINVERT));
    EXPECT_EQ(m[19], (Method{k2D, NV902D_SET_COLOR_KEY_ENABLE, 0}));
    EXPECT_EQ(m[27], (Method{k2D, NV902D_SET_RENDER_SOLID_PRIM_COLOR_FORMAT, NV902D_SET_DST_FORMAT_V_A8B8G8R8}));
    EXPECT_EQ(m[29], (Method{k2D, NV902D_RENDER_SOLID_PRIM_POINT_SET_X(0), 0}));
    EXPECT_EQ(m[32], (Method{k2D, NV902D_RENDER_SOLID_PRIM_POINT_Y(1), 2560}));
    EXPECT_EQ(m[33], (Method{k2D, NV902D_SET_CLIP_ENABLE, NV902D_SET_CLIP_ENABLE_V_FALSE}));
    EXPECT_EQ(m.back(), (Method{k2D, NV902D_SET_OPERATION, NV902D_SET_OPERATION_V_SRCCOPY}));
}

TEST(Emit2D, ARopScaleSetsTheRopBeforeTheLaunchAndSrccopyAfter)
{
    switch_gm20b_2d_surface src = {}, dst = {};
    src.format = NV902D_SET_SRC_FORMAT_V_A8R8G8B8;
    src.block_height_log2 = 4;
    src.width = 1440;
    src.height = 2560;
    dst = src;
    dst.width = 720;
    dst.height = 1280;
    const switch_gm20b_2d_scale_op plain = {0,          0,          720,  1280, 2ull << 32, 2ull << 32,
                                            1ull << 32, 1ull << 32, true, true, 0};
    switch_gm20b_2d_scale_op inverted = plain;
    inverted.rop = SWITCH_GM20B_2D_ROP_NOTSRCCOPY;
    std::vector<uint32_t> a(64), b(64);
    nv_push pa, pb;
    nv_push_init(&pa, a.data(), a.size(), SUBC_MASK_ALL);
    nv_push_init(&pb, b.data(), b.size(), SUBC_MASK_ALL);
    switch_gm20b_2d_scale(&pa, &src, &dst, &plain);
    switch_gm20b_2d_scale(&pb, &src, &dst, &inverted);
    b.resize(nv_push_dw_count(&pb));
    a.resize(nv_push_dw_count(&pa));
    EXPECT_LE(b.size(), SWITCH_GM20B_2D_SCALE_WORDS);
    std::vector<Method> expected = Methods(a);
    const auto rop = ropOn(SWITCH_GM20B_2D_ROP_NOTSRCCOPY);
    expected.insert(expected.begin() + 19, rop.begin(), rop.end());
    expected.push_back({k2D, NV902D_SET_OPERATION, NV902D_SET_OPERATION_V_SRCCOPY});
    EXPECT_EQ(Methods(b), expected);
}

}  // namespace
