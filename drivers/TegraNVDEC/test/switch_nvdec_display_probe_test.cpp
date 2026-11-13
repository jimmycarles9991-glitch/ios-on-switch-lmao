// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <string>
#include "../SwitchNVDECAccess.h"
#include "../SwitchNVDECDisplayProbe.h"

namespace {
constexpr uint32_t kWidth = 208, kHeight = 160, kColumns = 256, kRows = 192;
constexpr uint64_t kSlotLuma = uint64_t(kColumns) * 2 * kRows;
constexpr uint32_t kColMv = kColumns * kRows / 16;

struct Job {
    TegraNVDECSubmit submit = {};
    uint64_t avail[TEGRA_NVDEC_MAX_ENTRIES] = {};

    nvdec_hevc_pic_s &setup()
    {
        return *reinterpret_cast<nvdec_hevc_pic_s *>(submit.setup);
    }

    void add(uint16_t method, uint16_t kind, uint32_t value, uint64_t bytes)
    {
        avail[submit.entry_count] = bytes;
        submit.entries[submit.entry_count++] = {method, kind, value, 0};
    }
};

// A Main 10 picture as HEVCNVDEC sizes one in mode 1 (16-bit samples), its display buffer at 16-bit size, in mode 2.
Job mainTen(uint64_t displayLuma = kSlotLuma, uint64_t displayChroma = kSlotLuma / 2)
{
    Job j;
    j.submit.codec = TEGRA_NVDEC_CODEC_HEVC;
    j.submit.setup_bytes = sizeof(nvdec_hevc_pic_s);
    auto &p = j.setup();
    p.stream_len = 1000;
    p.disp_output_mode = 2;
    p.framestride[0] = p.framestride[1] = kColumns;
    p.colMvBuffersize = kColMv / 256;
    p.HevcSaoBufferOffset = 480 * 2 * kRows / 256;
    p.HevcBsdCtrlOffset = (480 + 3840) * 2 * kRows / 256;
    p.pic_width_in_luma_samples = kWidth;
    p.pic_height_in_luma_samples = kHeight;
    p.chroma_format_idc = 1;
    p.bit_depth_luma = p.bit_depth_chroma = 10;
    p.log2_min_luma_coding_block_size = 3;
    p.log2_max_luma_coding_block_size = 4;
    p.log2_min_transform_block_size = 2;
    p.log2_max_transform_block_size = 4;

    const uint64_t filter = uint64_t(p.HevcBsdCtrlOffset) * 256 + 60 * kRows;
    j.add(NVC5B0_SET_APPLICATION_ID, TEGRA_NVDEC_ENTRY_VALUE, TEGRA_NVDEC_CODEC_HEVC, 0);
    j.add(NVC5B0_SET_CONTROL_PARAMS, TEGRA_NVDEC_ENTRY_VALUE, TEGRA_NVDEC_CODEC_HEVC | 0x10, 0);
    j.add(NVC5B0_SET_IN_BUF_BASE_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, 1, 4096);
    j.add(NVC5B0_SET_NVDEC_STATUS_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, 2, sizeof(nvdec_status_s));
    j.add(NVC5B0_HEVC_SET_SCALING_LIST_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, 3, sizeof(nvdec_hevc_scaling_list_s));
    j.add(NVC5B0_HEVC_SET_TILE_SIZES_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, 4, 0x900);
    j.add(NVC5B0_HEVC_SET_FILTER_BUFFER_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, 5, filter);
    j.add(NVC5B0_SET_COLOC_DATA_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, 6, 17ull * kColMv);
    for (uint16_t i = 0; i < 16; i++) {
        j.add(NVC5B0_SET_PICTURE_LUMA_OFFSET0 + 4 * i, TEGRA_NVDEC_ENTRY_RELOC, 10 + i, kSlotLuma);
        j.add(NVC5B0_SET_PICTURE_CHROMA_OFFSET0 + 4 * i, TEGRA_NVDEC_ENTRY_RELOC, 10 + i, kSlotLuma / 2);
    }
    j.add(NVC5B0_SET_DISPLAY_BUF_LUMA_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, 40, displayLuma);
    j.add(NVC5B0_SET_DISPLAY_BUF_CHROMA_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, 41, displayChroma);
    j.add(NVC5B0_EXECUTE, TEGRA_NVDEC_ENTRY_VALUE, 0x100, 0);
    return j;
}

std::string check(Job &j)
{
    static TegraNVDECSubmit plain;
    static uint64_t plainAvail[TEGRA_NVDEC_MAX_ENTRIES];
    const char *why = switch_nvdec_check_display_probe(&j.submit, j.avail, &plain, plainAvail);
    return why ? why : "";
}

uint32_t indexOf(const Job &j, uint16_t method)
{
    for (uint32_t i = 0; i < j.submit.entry_count; i++)
        if (j.submit.entries[i].method == method)
            return i;
    return UINT32_MAX;
}
}  // namespace

TEST(SwitchNVDECDisplayProbe, TakesModesTwoAndThreeWithSixteenBitBuffers)
{
    Job j = mainTen();
    EXPECT_EQ(check(j), "");
    j.setup().disp_output_mode = 3;
    EXPECT_EQ(check(j), "");
    EXPECT_EQ(j.setup().disp_output_mode, 3) << "the job NVDEC runs keeps its mode";
}

std::string ordinary(Job &j)
{
    const char *why = tegra_nvdec_check(&j.submit, j.avail);
    return why ? why : "";
}

void insertBeforeExecute(Job &j, uint16_t method, uint16_t kind, uint32_t value, uint64_t bytes)
{
    const TegraNVDECEntry execute = j.submit.entries[--j.submit.entry_count];
    j.add(method, kind, value, bytes);
    j.add(execute.method, execute.kind, execute.value, 0);
}

TEST(SwitchNVDECDisplayProbe, TheOrdinaryCheckTakesModesTwoAndThreeWithOrWithoutTheDisplayBuffer)
{
    Job j = mainTen();
    EXPECT_EQ(ordinary(j), "");
    j.setup().disp_output_mode = 3;
    EXPECT_EQ(ordinary(j), "");
    j = mainTen();
    j.submit.entries[indexOf(j, NVC5B0_SET_DISPLAY_BUF_LUMA_OFFSET)].method = NVC5B0_SET_HISTORY_OFFSET;
    EXPECT_EQ(ordinary(j), "relocated method");
    Job bare = mainTen();
    bare.submit.entry_count -= 3;
    bare.add(NVC5B0_EXECUTE, TEGRA_NVDEC_ENTRY_VALUE, 0x100, 0);
    EXPECT_EQ(ordinary(bare), "");
}

TEST(SwitchNVDECDisplayProbe, TheOrdinaryCheckBoundsTheDisplayBufferAndItsSize)
{
    Job j = mainTen(kSlotLuma, kSlotLuma / 2 - 1);
    EXPECT_EQ(ordinary(j), "display buffer too small");
    j = mainTen();
    j.setup().disp_output_mode = 1;
    EXPECT_EQ(ordinary(j), "display buffer outside modes 2 and 3");
    j = mainTen();
    j.submit.entries[indexOf(j, NVC5B0_SET_DISPLAY_BUF_CHROMA_OFFSET)].method = NVC5B0_SET_DISPLAY_BUF_LUMA_OFFSET;
    EXPECT_EQ(ordinary(j), "repeated relocation");
    j = mainTen();
    insertBeforeExecute(j, NVC5B0_SET_DISPLAY_BUF_SIZE, TEGRA_NVDEC_ENTRY_VALUE, uint32_t(kSlotLuma / 2), 0);
    EXPECT_EQ(ordinary(j), "value method");
}

TEST(SwitchNVDECDisplayProbe, RefusesWhatIsNotAMainTenProbe)
{
    Job j = mainTen();
    j.setup().disp_output_mode = 1;
    EXPECT_EQ(check(j), "probe: not Main 10 in mode 2 or 3");
    j = mainTen();
    j.setup().bit_depth_luma = j.setup().bit_depth_chroma = 8;
    EXPECT_EQ(check(j), "probe: not Main 10 in mode 2 or 3");
    j = mainTen();
    j.submit.codec = TEGRA_NVDEC_CODEC_H264;
    EXPECT_EQ(check(j), "probe: not an HEVC submit");
}

TEST(SwitchNVDECDisplayProbe, BoundsTheDisplayBufferAtSixteenBitSamples)
{
    Job j = mainTen(kSlotLuma / 2, kSlotLuma / 4);
    EXPECT_EQ(check(j), "probe: display plane smaller than the 16-bit picture's");
    j = mainTen(kSlotLuma, kSlotLuma / 2 - 1);
    EXPECT_EQ(check(j), "probe: display plane smaller than the 16-bit picture's");
}

TEST(SwitchNVDECDisplayProbe, RefusesAMissingRepeatedOrMisalignedDisplayBuffer)
{
    Job j = mainTen();
    j.submit.entries[indexOf(j, NVC5B0_SET_DISPLAY_BUF_CHROMA_OFFSET)].method = NVC5B0_SET_DISPLAY_BUF_LUMA_OFFSET;
    EXPECT_EQ(check(j), "probe: repeated display buffer");
    j = mainTen();
    j.submit.entries[indexOf(j, NVC5B0_SET_DISPLAY_BUF_LUMA_OFFSET)].offset = 0x80;
    EXPECT_EQ(check(j), "probe: display buffer relocation");
    j = mainTen();
    j.submit.entries[indexOf(j, NVC5B0_SET_DISPLAY_BUF_LUMA_OFFSET)].kind = TEGRA_NVDEC_ENTRY_VALUE;
    EXPECT_EQ(check(j), "probe: display buffer relocation");
    j = mainTen();
    j.submit.entries[indexOf(j, NVC5B0_SET_DISPLAY_BUF_CHROMA_OFFSET)].method = NVC5B0_SET_HISTORY_OFFSET;
    EXPECT_EQ(check(j), "probe: display buffer not bound");
}

TEST(SwitchNVDECDisplayProbe, SlotsAndFilterRowsMustBeSixteenBit)
{
    Job j = mainTen();
    j.avail[indexOf(j, NVC5B0_SET_PICTURE_LUMA_OFFSET0 + 4 * 5)] = kSlotLuma / 2;
    EXPECT_EQ(check(j), "surface too small");
    j = mainTen();
    j.setup().HevcBsdCtrlOffset = (480 + 3840) * kRows / 256;
    EXPECT_EQ(check(j), "filter buffer");
}

TEST(SwitchNVDECDisplayProbe, DisplayBufferSizeStaysRefused)
{
    Job j = mainTen();
    const uint32_t execute = indexOf(j, NVC5B0_EXECUTE);
    j.submit.entries[execute] = {NVC5B0_SET_DISPLAY_BUF_SIZE, TEGRA_NVDEC_ENTRY_VALUE, 1, 0};
    j.add(NVC5B0_EXECUTE, TEGRA_NVDEC_ENTRY_VALUE, 0x100, 0);
    EXPECT_EQ(check(j), "value method");
    EXPECT_EQ(ordinary(j), "value method");
}

TEST(SwitchNVDECAccess, TheDisplayBufferIsWritten)
{
    Job j = mainTen();
    EXPECT_EQ(switch_nvdec_entry_access(j.submit, indexOf(j, NVC5B0_SET_DISPLAY_BUF_LUMA_OFFSET)), SWITCH_NVDEC_WRITE);
    EXPECT_EQ(switch_nvdec_entry_access(j.submit, indexOf(j, NVC5B0_SET_DISPLAY_BUF_CHROMA_OFFSET)),
              SWITCH_NVDEC_WRITE);
}
