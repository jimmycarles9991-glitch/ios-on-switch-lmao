// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <string.h>
#include "../TegraVICCompose.h"
#include "libdrm/vic40.h"

namespace {
constexpr uint64_t kOne = 1ull << 16;

TegraVICImage nv12(uint64_t base, uint32_t width, uint32_t height, uint32_t standard = TEGRA_VIC_YCC_709)
{
    TegraVICImage im = {};
    im.format = tegra_vic_format(TEGRA_VIC_FOURCC('4', '2', '0', 'v'));
    im.width = width;
    im.height = height;
    im.pitch[0] = im.pitch[1] = (width + 255) / 256 * 256;
    im.rows[0] = height;
    im.rows[1] = height / 2;
    im.base[0] = base;
    im.base[1] = base + uint64_t(im.pitch[0]) * height;
    im.span[0] = im.span[1] = uint64_t(im.pitch[0]) * height;
    im.standard = standard;
    return im;
}

TegraVICImage bgra(uint64_t base, uint32_t width, uint32_t height, uint32_t align = 256)
{
    TegraVICImage im = {};
    im.format = tegra_vic_format(TEGRA_VIC_FOURCC('B', 'G', 'R', 'A'));
    im.width = width;
    im.height = height;
    im.pitch[0] = (width * 4 + align - 1) / align * align;
    im.rows[0] = height;
    im.base[0] = base;
    im.span[0] = uint64_t(im.pitch[0]) * height;
    return im;
}

TegraVICComposeSlot slot(const TegraVICImage &source, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         uint32_t blend = TEGRA_VIC_BLEND_OPAQUE)
{
    TegraVICComposeSlot s = {};
    s.source = &source;
    s.srcW = uint64_t(source.width) << 16;
    s.srcH = uint64_t(source.height) << 16;
    s.dstX = x;
    s.dstY = y;
    s.dstW = w;
    s.dstH = h;
    s.blend = blend;
    s.alpha10 = 1023;
    return s;
}

ConfigStruct configOf(const TegraVICJob &job)
{
    ConfigStruct c;
    static_assert(sizeof(c) == TEGRA_VIC_CONFIG_BYTES);
    memcpy(&c, job.config, sizeof(c));
    return c;
}

TEST(TegraVICCompose, BlendFieldsLandWhereVic40PutsThem)
{
    for (uint32_t s = 0; s < 8; ++s) {
        const uint32_t base = TEGRA_VIC_SLOTS + s * TEGRA_VIC_SLOT_BYTES + TEGRA_VIC_SLOT_BLEND;
        const auto check = [&](auto assign, uint32_t bit, uint32_t width) {
            ConfigStruct ref;
            uint8_t ours[TEGRA_VIC_CONFIG_BYTES];
            memset(&ref, 0, sizeof(ref));
            memset(ours, 0, sizeof(ours));
            assign(ref.slotStruct[s].blendingSlotStruct);
            tegra_vic_put(ours, base, bit, width, (1ull << width) - 1);
            EXPECT_EQ(memcmp(&ref, ours, sizeof(ours)), 0) << "slot " << s << " bit " << bit;
        };
        check(
            [](BlendingSlotStruct &b) {
                b.AlphaK1 = 1023;
            },
            SLOT_BLEND_K1_BIT, SLOT_BLEND_K1_WIDTH);
        check(
            [](BlendingSlotStruct &b) {
                b.AlphaK2 = 1023;
            },
            SLOT_BLEND_K2_BIT, SLOT_BLEND_K2_WIDTH);
        check(
            [](BlendingSlotStruct &b) {
                b.SrcFactCMatchSelect = 7;
            },
            SLOT_BLEND_SRC_C_BIT, SLOT_BLEND_SRC_C_WIDTH);
        check(
            [](BlendingSlotStruct &b) {
                b.DstFactCMatchSelect = 7;
            },
            SLOT_BLEND_DST_C_BIT, SLOT_BLEND_DST_C_WIDTH);
        check(
            [](BlendingSlotStruct &b) {
                b.SrcFactAMatchSelect = 7;
            },
            SLOT_BLEND_SRC_A_BIT, SLOT_BLEND_SRC_A_WIDTH);
        check(
            [](BlendingSlotStruct &b) {
                b.DstFactAMatchSelect = 7;
            },
            SLOT_BLEND_DST_A_BIT, SLOT_BLEND_DST_A_WIDTH);
    }
}

TEST(TegraVICCompose, OneOpaqueSlotIsTheSingleSlotJobPlusItsBlend)
{
    const TegraVICImage src = nv12(0x10000000, 640, 360), dst = bgra(0x20000000, 640, 360, 64);
    TegraVICOp op = {};
    op.srcW = uint64_t(640) << 16;
    op.srcH = uint64_t(360) << 16;
    op.dstW = 640;
    op.dstH = 360;
    TegraVICJob single, composed;
    ASSERT_EQ(tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, 0x1000, 0x2000, &single), nullptr);
    const TegraVICComposeSlot s = slot(src, 0, 0, 640, 360);
    const TegraVICRect target = {0, 0, 640, 360};
    ASSERT_EQ(tegra_vic_compose_build(&s, 1, &dst, &target, &kTegraVICTraits, 0x1000, 0x2000, &composed), nullptr);
    const uint32_t blend = TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_BLEND;
    memset(composed.config + blend, 0, 16);
    EXPECT_EQ(memcmp(single.config, composed.config, TEGRA_VIC_CONFIG_BYTES), 0);
    ASSERT_EQ(single.count, composed.count);
    for (uint32_t i = 0; i < single.count; ++i) {
        EXPECT_EQ(single.method[i], composed.method[i]) << i;
        EXPECT_EQ(single.value[i], composed.value[i]) << i;
    }
    EXPECT_EQ(single.pixels, composed.pixels);
}

TEST(TegraVICCompose, TwoSlotsGetTheirOwnSurfacesRectanglesMatricesAndBlends)
{
    const TegraVICImage video = nv12(0x10000000, 3840, 2160, TEGRA_VIC_YCC_709);
    const TegraVICImage ui = bgra(0x18000000, 300, 200);
    const TegraVICImage dst = bgra(0x20000000, 720, 1280, 64);
    TegraVICComposeSlot slots[2] = {slot(video, 0, 100, 720, 405),
                                    slot(ui, 200, 300, 300, 200, TEGRA_VIC_BLEND_PREMULTIPLIED)};
    slots[1].alpha10 = 512;
    const TegraVICRect target = {0, 100, 720, 405};
    TegraVICJob job;
    ASSERT_EQ(tegra_vic_compose_build(slots, 2, &dst, &target, &kTegraVICTraits, 0x1000, 0x2000, &job), nullptr);
    const ConfigStruct c = configOf(job);
    const auto &bottom = c.slotStruct[0], &top = c.slotStruct[1];
    EXPECT_EQ(bottom.slotConfig.SlotEnable, 1u);
    EXPECT_EQ(top.slotConfig.SlotEnable, 1u);
    EXPECT_EQ(c.slotStruct[2].slotConfig.SlotEnable, 0u);
    EXPECT_EQ(bottom.slotConfig.SourceRectRight, (3840 - 1) * kOne);
    EXPECT_EQ(bottom.slotConfig.DestRectTop, 100u);
    EXPECT_EQ(bottom.slotConfig.DestRectBottom, 504u);
    EXPECT_EQ(top.slotConfig.DestRectLeft, 200u);
    EXPECT_EQ(top.slotConfig.DestRectRight, 499u);
    EXPECT_EQ(bottom.slotSurfaceConfig.SlotPixelFormat, uint64_t(TEGRA_VIC_T_Y8___U8V8_N420));
    EXPECT_EQ(top.slotSurfaceConfig.SlotPixelFormat, uint64_t(TEGRA_VIC_T_A8R8G8B8));
    EXPECT_EQ(bottom.colorMatrixStruct.matrix_enable, 1u);
    EXPECT_EQ(top.colorMatrixStruct.matrix_enable, 0u);
    EXPECT_EQ(bottom.slotConfig.ConstantAlpha, 1u);
    EXPECT_EQ(top.slotConfig.ConstantAlpha, 0u);
    EXPECT_EQ(top.blendingSlotStruct.AlphaK1, 512u);
    EXPECT_EQ(top.blendingSlotStruct.SrcFactCMatchSelect, uint64_t(TEGRA_VIC_SRC_C_K1));
    EXPECT_EQ(top.blendingSlotStruct.DstFactCMatchSelect, uint64_t(TEGRA_VIC_DST_C_ONE_MINUS_K1_SRC_ALPHA));
    EXPECT_EQ(top.blendingSlotStruct.DstFactAMatchSelect, uint64_t(TEGRA_VIC_DST_A_ONE_MINUS_K1_SRC_ALPHA));
    EXPECT_EQ(c.outputConfig.AlphaFillMode, uint64_t(TEGRA_VIC_ALPHA_OPAQUE));
    EXPECT_EQ(c.outputConfig.BackgroundAlpha, 1023u);
    EXPECT_EQ(c.outputConfig.BackgroundR | c.outputConfig.BackgroundG | c.outputConfig.BackgroundB, 0u);
    EXPECT_EQ(c.outputConfig.TargetRectTop, 100u);
    EXPECT_EQ(c.outputConfig.TargetRectBottom, 504u);
    const uint32_t methods[] = {TEGRA_VIC_M_SET_APPLICATION_ID,
                                TEGRA_VIC_M_SET_CONTROL_PARAMS,
                                TEGRA_VIC_M_SET_CONFIG_STRUCT_OFFSET,
                                TEGRA_VIC_M_SET_FILTER_STRUCT_OFFSET,
                                TEGRA_VIC_M_OUTPUT_LUMA,
                                0x400,
                                0x404,
                                0x460,
                                TEGRA_VIC_M_EXECUTE};
    const uint32_t values[] = {
        1, 0, 0x10, 0x20, 0x200000, 0x100000, uint32_t(video.base[1] >> 8), 0x180000, TEGRA_VIC_EXECUTE_AWAKEN};
    ASSERT_EQ(job.count, sizeof(methods) / sizeof(methods[0]));
    for (uint32_t i = 0; i < job.count; ++i) {
        EXPECT_EQ(job.method[i], methods[i]) << i;
        if (methods[i] != TEGRA_VIC_M_SET_CONTROL_PARAMS)
            EXPECT_EQ(job.value[i], values[i]) << i;
    }
    EXPECT_EQ(job.pixels, 720ull * 405 + 3840ull * 2160 + 300ull * 200);
}

TEST(TegraVICCompose, StraightAlphaMultipliesColourBySourceAlpha)
{
    const TegraVICImage a = bgra(0x10000000, 64, 64), dst = bgra(0x20000000, 64, 64, 64);
    const TegraVICComposeSlot s = slot(a, 0, 0, 64, 64, TEGRA_VIC_BLEND_STRAIGHT);
    const TegraVICRect target = {0, 0, 64, 64};
    TegraVICJob job;
    ASSERT_EQ(tegra_vic_compose_build(&s, 1, &dst, &target, &kTegraVICTraits, 0, 0, &job), nullptr);
    EXPECT_EQ(configOf(job).slotStruct[0].blendingSlotStruct.SrcFactCMatchSelect,
              uint64_t(TEGRA_VIC_SRC_C_K1_SRC_ALPHA));
}

TEST(TegraVICCompose, BlockLinearDestinationIsWholeBlocksOfSixteenGobs)
{
    const TegraVICImage a = bgra(0x10000000, 720, 405);
    TegraVICImage dst = bgra(0x20000000, 720, 405, 64);
    dst.blockLinear = 1;
    dst.blockHeightLog2 = 4;
    dst.rows[0] = 512;
    dst.span[0] = uint64_t(dst.pitch[0]) * 512;
    const TegraVICComposeSlot s = slot(a, 0, 0, 720, 405);
    const TegraVICRect target = {0, 0, 720, 405};
    TegraVICJob job;
    ASSERT_EQ(tegra_vic_compose_build(&s, 1, &dst, &target, &kTegraVICTraits, 0, 0, &job), nullptr);
    const ConfigStruct c = configOf(job);
    EXPECT_EQ(c.outputSurfaceConfig.OutBlkKind, uint64_t(TEGRA_VIC_BLK_16Bx2));
    EXPECT_EQ(c.outputSurfaceConfig.OutBlkHeight, 4u);
    EXPECT_EQ(c.outputSurfaceConfig.OutLumaHeight, 511u);
    EXPECT_EQ(c.outputSurfaceConfig.OutLumaWidth, 719u);
    TegraVICTraits pitchOnly = kTegraVICTraits;
    pitchOnly.blockLinearOut = 0;
    EXPECT_STREQ(tegra_vic_compose_build(&s, 1, &dst, &target, &pitchOnly, 0, 0, &job),
                 "block-linear destination this engine has not proven");
}

TEST(TegraVICCompose, RefusesWhatOneJobCannotCarry)
{
    const TegraVICImage v = nv12(0x10000000, 640, 360), dst = bgra(0x20000000, 720, 1280, 64);
    TegraVICComposeSlot slots[4] = {slot(v, 0, 0, 640, 360), slot(v, 0, 360, 640, 360), slot(v, 0, 720, 640, 360),
                                    slot(v, 0, 900, 640, 360)};
    const TegraVICRect target = {0, 0, 720, 1280};
    TegraVICJob job;
    EXPECT_EQ(tegra_vic_compose_build(slots, 3, &dst, &target, &kTegraVICTraits, 0, 0, &job), nullptr);
    EXPECT_EQ(job.count, uint32_t(TEGRA_VIC_MAX_METHODS));
    EXPECT_STREQ(tegra_vic_compose_build(slots, 4, &dst, &target, &kTegraVICTraits, 0, 0, &job),
                 "more surface methods than one job holds");
    EXPECT_STREQ(tegra_vic_compose_build(slots, 0, &dst, &target, &kTegraVICTraits, 0, 0, &job),
                 "slot count outside 1..8");
    TegraVICTraits one = kTegraVICTraits;
    one.multiSlot = 0;
    EXPECT_STREQ(tegra_vic_compose_build(slots, 2, &dst, &target, &one, 0, 0, &job),
                 "multi-slot composition this engine has not proven");
    EXPECT_EQ(tegra_vic_compose_build(slots, 1, &dst, &target, &one, 0, 0, &job), nullptr);
    TegraVICTraits opaqueOnly = kTegraVICTraits;
    opaqueOnly.slotAlpha = 0;
    EXPECT_EQ(tegra_vic_compose_build(slots, 2, &dst, &target, &opaqueOnly, 0, 0, &job), nullptr);
    TegraVICComposeSlot half = slots[1];
    half.alpha10 = 512;
    EXPECT_STREQ(tegra_vic_compose_build(&half, 1, &dst, &target, &opaqueOnly, 0, 0, &job),
                 "slot alpha blending this engine has not proven");
    TegraVICComposeSlot outside = slot(v, 100, 1000, 640, 360);
    EXPECT_STREQ(tegra_vic_compose_build(&outside, 1, &dst, &target, &kTegraVICTraits, 0, 0, &job),
                 "slot rectangle outside the destination");
    TegraVICComposeSlot premultipliedVideo = slot(v, 0, 0, 640, 360, TEGRA_VIC_BLEND_PREMULTIPLIED);
    EXPECT_STREQ(tegra_vic_compose_build(&premultipliedVideo, 1, &dst, &target, &kTegraVICTraits, 0, 0, &job),
                 "alpha blend of a source without alpha");
    TegraVICComposeSlot tiny = slot(v, 0, 0, 20, 20);
    EXPECT_STREQ(tegra_vic_compose_build(&tiny, 1, &dst, &target, &kTegraVICTraits, 0, 0, &job), "downscale past 16x");
    const TegraVICImage yccOut = nv12(0x20000000, 720, 1280);
    EXPECT_STREQ(tegra_vic_compose_build(slots, 1, &yccOut, &target, &kTegraVICTraits, 0, 0, &job),
                 "composition into a format other than RGB");
    const TegraVICRect past = {0, 1000, 720, 400};
    EXPECT_STREQ(tegra_vic_compose_build(slots, 1, &dst, &past, &kTegraVICTraits, 0, 0, &job),
                 "target rectangle outside the destination");
}

TEST(TegraVICCompose, SlotsDoNotCompareTransferOrPrimaries)
{
    TegraVICImage pq = nv12(0x10000000, 640, 360), sdr = nv12(0x14000000, 640, 360);
    pq.transfer = 16;
    pq.primaries = 9;
    const TegraVICImage dst = bgra(0x20000000, 720, 720, 64);
    const TegraVICComposeSlot slots[2] = {slot(pq, 0, 0, 640, 360), slot(sdr, 0, 360, 640, 360)};
    const TegraVICRect target = {0, 0, 720, 720};
    TegraVICJob job;
    EXPECT_EQ(tegra_vic_compose_build(slots, 2, &dst, &target, &kTegraVICTraits, 0, 0, &job), nullptr);
}
}  // namespace
