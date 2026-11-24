// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include "bazel/testing/source_text.h"
extern "C" {
#include "drivers/SwitchVideoEncoders/AVE/H264NVENC.h"
#include "drivers/SwitchVideoEncoders/AVE/HEVCNVENC.h"
}
#include "drivers/TegraNVENC/TegraNVENCCheck.h"
#include "drivers/TegraNVENC/TegraNVENCPitchPlan.h"

namespace {
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

constexpr H264EncHandles kHandles = {.input = 1, .recon = 2, .ref = 3, .bitstream = 4, .aux = 5, .mepred = {6, 7}};

void computeAvail(const TegraNVENCSubmit &s, const H264EncLayout &l, uint64_t scratchBytes,
                  uint64_t (&avail)[TEGRA_NVENC_MAX_ENTRIES])
{
    const uint64_t sizes[8] = {
        0, l.input.bytes, l.recon.bytes, l.recon.bytes, l.bitstream_bytes, l.aux_bytes, l.mepred_bytes, l.mepred_bytes,
    };
    memset(avail, 0, sizeof(avail));
    for (uint32_t i = 0; i < s.entry_count; i++) {
        if (s.entries[i].kind != TEGRA_NVENC_ENTRY_RELOC)
            continue;
        const uint64_t size = s.entries[i].value == TEGRA_NVENC_PITCH_HANDLE ? scratchBytes : sizes[s.entries[i].value];
        ASSERT_LE(s.entries[i].offset, size);
        avail[i] = size - s.entries[i].offset;
    }
}

TEST(NVENCPitchPlan, ConvertsH264PitchSubmitToValidBL2Scratch)
{
    const uint32_t chromaModes[2] = {TEGRA_NVENC_CHROMA_OFFSET, TEGRA_NVENC_CHROMA_METHOD};
    for (uint32_t chromaMode : chromaModes) {
        H264EncConfig c = {};
        c.width = 1280;
        c.height = 720;
        c.profile_idc = H264_PROFILE_HIGH;
        c.cabac = true;
        c.transform8x8 = true;
        c.fps_num = 30;
        c.fps_den = 1;
        c.input_layout = TEGRA_NVENC_LAYOUT_PITCH;
        c.chroma_mode = chromaMode;
        ASSERT_EQ(h264enc_configure(&c), 0);

        H264EncLayout l = {};
        h264enc_layout(&c, &l);
        const H264EncPicture idr = {.type = H264ENC_IDR, .qp = 26};
        TegraNVENCSubmit submit = {};
        h264enc_fill_submit(&c, &l, &idr, &kHandles, &submit);
        submit.clean[submit.clean_count++] = {kHandles.aux, 0, 256, 0};

        EXPECT_TRUE(tegra_nvenc_is_pitch_input(&submit));
        TegraNVENCPitchPlan plan = {};
        EXPECT_EQ(tegra_nvenc_pitch_plan(&submit, &plan), nullptr);
        EXPECT_EQ(plan.inputHandle, kHandles.input);
        EXPECT_EQ(plan.width, 1280u);
        EXPECT_EQ(plan.height, 720u);
        EXPECT_EQ(plan.srcPitch, 1280u);
        EXPECT_EQ(plan.srcLumaOffset, 0u);
        EXPECT_EQ(plan.srcChromaOffset, l.input.chroma_offset);
        EXPECT_EQ(plan.bl2Pitch, 1280u);
        EXPECT_EQ(plan.bl2LumaRows, 768u);
        EXPECT_EQ(plan.bl2ChromaRows, 384u);

        TegraNVENCSubmit patched = {};
        tegra_nvenc_pitch_patch(&submit, &plan, TEGRA_NVENC_PITCH_HANDLE, &patched);
        EXPECT_FALSE(tegra_nvenc_is_pitch_input(&patched));
        ASSERT_EQ(patched.clean_count, 1u);
        EXPECT_EQ(patched.clean[0].handle, kHandles.aux);

        nvenc_h264_surface_cfg_s cfg;
        memcpy(&cfg, patched.setup + offsetof(nvenc_h264_drv_pic_setup_s, input_cfg), sizeof(cfg));
        EXPECT_EQ(cfg.block_height, 2u);
        EXPECT_EQ(cfg.tiled_16x16, 0u);
        EXPECT_EQ(cfg.chroma_top_frm_offset, 0u);

        uint64_t avail[TEGRA_NVENC_MAX_ENTRIES];
        computeAvail(patched, l, plan.scratchBytes, avail);
        EXPECT_EQ(tegra_nvenc_check(&patched, avail), nullptr);
    }
}

TEST(NVENCPitchPlan, ConvertsH265PitchSubmitToValidBL2Scratch)
{
    HEVCEncConfig c = {};
    c.width = 1280;
    c.height = 720;
    c.fps_num = 30;
    c.fps_den = 1;
    ASSERT_EQ(hevcnvenc_configure(&c), 0);

    H264EncLayout l = {};
    hevcnvenc_layout(&c, TEGRA_NVENC_LAYOUT_PITCH, &l);
    const HEVCEncPicture idr = {.idr = true, .qp = 26};
    TegraNVENCSubmit submit = {};
    hevcnvenc_fill_submit(&c, TEGRA_NVENC_LAYOUT_PITCH, TEGRA_NVENC_CHROMA_METHOD, &l, &idr, &kHandles, &submit);

    EXPECT_TRUE(tegra_nvenc_is_pitch_input(&submit));
    TegraNVENCPitchPlan plan = {};
    EXPECT_EQ(tegra_nvenc_pitch_plan(&submit, &plan), nullptr);
    TegraNVENCSubmit patched = {};
    tegra_nvenc_pitch_patch(&submit, &plan, TEGRA_NVENC_PITCH_HANDLE, &patched);
    EXPECT_FALSE(tegra_nvenc_is_pitch_input(&patched));

    uint64_t avail[TEGRA_NVENC_MAX_ENTRIES];
    computeAvail(patched, l, plan.scratchBytes, avail);
    EXPECT_EQ(tegra_nvenc_check(&patched, avail), nullptr);
}

TEST(NVENCPitchPlan, RefusesNonPitchSeparateBuffersOverlappingPlanesAndUnsupportedModes)
{
    H264EncConfig c = {};
    c.width = 1280;
    c.height = 720;
    c.profile_idc = H264_PROFILE_HIGH;
    c.cabac = true;
    c.transform8x8 = true;
    c.fps_num = 30;
    c.fps_den = 1;
    c.input_layout = TEGRA_NVENC_LAYOUT_BL2;
    ASSERT_EQ(h264enc_configure(&c), 0);

    H264EncLayout l = {};
    h264enc_layout(&c, &l);
    const H264EncPicture idr = {.type = H264ENC_IDR, .qp = 26};
    TegraNVENCSubmit bl2 = {};
    h264enc_fill_submit(&c, &l, &idr, &kHandles, &bl2);
    EXPECT_FALSE(tegra_nvenc_is_pitch_input(&bl2));
    TegraNVENCPitchPlan plan = {};
    EXPECT_NE(tegra_nvenc_pitch_plan(&bl2, &plan), nullptr);

    c.input_layout = TEGRA_NVENC_LAYOUT_PITCH;
    h264enc_layout(&c, &l);
    TegraNVENCSubmit pitch = {};
    h264enc_fill_submit(&c, &l, &idr, &kHandles, &pitch);

    TegraNVENCSubmit separate = pitch;
    for (uint32_t i = 0; i < separate.entry_count; i++) {
        if (separate.entries[i].method == NVC5B7_SET_IN_CUR_PIC_CHROMA_U)
            separate.entries[i].value = 99;
    }
    EXPECT_NE(tegra_nvenc_pitch_plan(&separate, &plan), nullptr);

    TegraNVENCSubmit overlap = pitch;
    nvenc_h264_surface_cfg_s cfg;
    memcpy(&cfg, overlap.setup + offsetof(nvenc_h264_drv_pic_setup_s, input_cfg), sizeof(cfg));
    cfg.chroma_top_frm_offset = 1;
    memcpy(overlap.setup + offsetof(nvenc_h264_drv_pic_setup_s, input_cfg), &cfg, sizeof(cfg));
    EXPECT_NE(tegra_nvenc_pitch_plan(&overlap, &plan), nullptr);

    TegraNVENCSubmit nv21 = pitch;
    memcpy(&cfg, nv21.setup + offsetof(nvenc_h264_drv_pic_setup_s, input_cfg), sizeof(cfg));
    cfg.nv21_enable = 1;
    memcpy(nv21.setup + offsetof(nvenc_h264_drv_pic_setup_s, input_cfg), &cfg, sizeof(cfg));
    EXPECT_NE(tegra_nvenc_pitch_plan(&nv21, &plan), nullptr);

    TegraNVENCSubmit badCounts = pitch;
    badCounts.entry_count = TEGRA_NVENC_MAX_ENTRIES + 1;
    EXPECT_NE(tegra_nvenc_pitch_plan(&badCounts, &plan), nullptr);

    TegraNVENCSubmit badField = pitch;
    memcpy(&cfg, badField.setup + offsetof(nvenc_h264_drv_pic_setup_s, input_cfg), sizeof(cfg));
    cfg.luma_bot_offset = 1;
    memcpy(badField.setup + offsetof(nvenc_h264_drv_pic_setup_s, input_cfg), &cfg, sizeof(cfg));
    EXPECT_NE(tegra_nvenc_pitch_plan(&badField, &plan), nullptr);

    TegraNVENCSubmit badDim = pitch;
    memcpy(&cfg, badDim.setup + offsetof(nvenc_h264_drv_pic_setup_s, input_cfg), sizeof(cfg));
    cfg.frame_width_minus1 = TEGRA_NVENC_MAX_DIMENSION + 1;
    memcpy(badDim.setup + offsetof(nvenc_h264_drv_pic_setup_s, input_cfg), &cfg, sizeof(cfg));
    EXPECT_NE(tegra_nvenc_pitch_plan(&badDim, &plan), nullptr);
}

TEST(NVENCPitchPlan, PreservesUnrelatedCleanRangeSharingInputHandle)
{
    H264EncConfig c = {};
    c.width = 1280;
    c.height = 720;
    c.profile_idc = H264_PROFILE_HIGH;
    c.cabac = true;
    c.transform8x8 = true;
    c.fps_num = 30;
    c.fps_den = 1;
    c.input_layout = TEGRA_NVENC_LAYOUT_PITCH;
    ASSERT_EQ(h264enc_configure(&c), 0);

    H264EncLayout l = {};
    h264enc_layout(&c, &l);
    const H264EncPicture idr = {.type = H264ENC_IDR, .qp = 26};
    TegraNVENCSubmit submit = {};
    h264enc_fill_submit(&c, &l, &idr, &kHandles, &submit);
    for (uint32_t i = 0; i < submit.entry_count; i++) {
        if (submit.entries[i].method == NVC5B7_SET_OUT_ENC_STATUS) {
            submit.entries[i].value = kHandles.input;
            submit.entries[i].offset = 4u << 20;
        }
    }
    submit.clean[submit.clean_count++] = {kHandles.input, 4u << 20, 256, 0};

    TegraNVENCPitchPlan plan = {};
    ASSERT_EQ(tegra_nvenc_pitch_plan(&submit, &plan), nullptr);
    TegraNVENCSubmit patched = {};
    tegra_nvenc_pitch_patch(&submit, &plan, TEGRA_NVENC_PITCH_HANDLE, &patched);
    ASSERT_EQ(patched.clean_count, 2u);
    EXPECT_EQ(patched.clean[1].handle, kHandles.input);
    EXPECT_EQ(patched.clean[1].offset, 4u << 20);
    EXPECT_EQ(patched.clean[1].length, 256u);
}

TEST(NVENCPitchPlan, SourcePatchConvertsPitchConfigToBL2)
{
    H264EncConfig c = {};
    c.width = 1280;
    c.height = 720;
    c.profile_idc = H264_PROFILE_HIGH;
    c.cabac = true;
    c.transform8x8 = true;
    c.fps_num = 30;
    c.fps_den = 1;
    c.input_layout = TEGRA_NVENC_LAYOUT_PITCH;
    ASSERT_EQ(h264enc_configure(&c), 0);
    H264EncLayout l = {};
    h264enc_layout(&c, &l);
    const H264EncPicture idr = {.type = H264ENC_IDR, .qp = 26};
    TegraNVENCSubmit pitch = {}, patched = {};
    h264enc_fill_submit(&c, &l, &idr, &kHandles, &pitch);

    tegra_nvenc_source_bl2_patch(&pitch, &patched);
    EXPECT_FALSE(tegra_nvenc_is_pitch_input(&patched));
    nvenc_h264_surface_cfg_s cfg;
    memcpy(&cfg, patched.setup + offsetof(nvenc_h264_drv_pic_setup_s, input_cfg), sizeof(cfg));
    EXPECT_EQ(cfg.block_height, 2u);
}

TEST(NVENCPitchWiring, KernelConvertsPitchInputsViaHardwareVICDMAAndNeverWritesPitch)
{
    const std::string source = SourceText("../TegraNVENCSource.cpp");
    EXPECT_THAT(source, HasSubstr("switch_vic_encode_buffer("));
    const std::string srcBody = Body(source, "int tegra_nvenc_source(");
    EXPECT_THAT(srcBody, HasSubstr("input.blockLinear = 1;"));
    EXPECT_THAT(srcBody, HasSubstr("tegra_nvenc_read_rows(height, blockRows)"));

    const std::string engine = SourceText("../TegraNVENC.cpp");
    EXPECT_THAT(Body(engine, "int check("), HasSubstr("clean_count"));

    const std::string client = SourceText("../TegraNVENCUserClient.cpp");
    EXPECT_THAT(client, HasSubstr("tegra_nvenc_is_pitch_input("));
    EXPECT_THAT(client, HasSubstr("tegra_nvenc_pitch_plan("));
    EXPECT_THAT(client, HasSubstr("tegra_nvenc_pitch_convert("));
    EXPECT_THAT(client, HasSubstr("tegra_nvenc_pitch_patch("));
    EXPECT_THAT(client, HasSubstr("tegra_nvenc_source_bl2_patch("));
    EXPECT_THAT(Body(client, "int AppleAVE2UserClient::preparePitch("), HasSubstr("tegra_nvenc_wedged()"));
}
}  // namespace
