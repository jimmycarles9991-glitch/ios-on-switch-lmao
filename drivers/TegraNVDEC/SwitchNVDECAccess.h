// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "TegraNVDECABI.h"
#include "nvidia/nvdec_drv.h"
#include "nvidia/clc5b0.h"

enum : uint32_t {
    SWITCH_NVDEC_READ = 1u,
    SWITCH_NVDEC_WRITE = 2u,
};

inline uint32_t switch_nvdec_entry_access(const TegraNVDECSubmit &submit, uint32_t index)
{
    if (index >= submit.entry_count || submit.entry_count > TEGRA_NVDEC_MAX_ENTRIES)
        return 0;
    const auto &entry = submit.entries[index];
    if (entry.kind != TEGRA_NVDEC_ENTRY_RELOC)
        return 0;
    uint32_t current = 0, slots = 0;
    switch (submit.codec) {
    case TEGRA_NVDEC_CODEC_H264:
        if (submit.setup_bytes != sizeof(nvdec_h264_pic_s))
            return 0;
        current = reinterpret_cast<const nvdec_h264_pic_s *>(submit.setup)->CurrPicIdx;
        slots = 17;
        break;
    case TEGRA_NVDEC_CODEC_HEVC:
        if (submit.setup_bytes != sizeof(nvdec_hevc_pic_s))
            return 0;
        current = reinterpret_cast<const nvdec_hevc_pic_s *>(submit.setup)->curr_pic_idx;
        slots = 16;
        break;
    case TEGRA_NVDEC_CODEC_VP9:
        if (submit.setup_bytes != sizeof(nvdec_vp9_pic_s))
            return 0;
        current = 3;
        slots = 4;
        break;
    case TEGRA_NVDEC_CODEC_MPEG4:
    case TEGRA_NVDEC_CODEC_MPEG12:
        if (submit.setup_bytes !=
            (submit.codec == TEGRA_NVDEC_CODEC_MPEG4 ? sizeof(nvdec_mpeg4_pic_s) : sizeof(nvdec_mpeg2_pic_s)))
            return 0;
        current = 0;
        slots = 3;
        break;
    default:
        return 0;
    }
    if (current >= slots)
        return 0;
    const uint32_t planes[] = {NVC5B0_SET_PICTURE_LUMA_OFFSET0, NVC5B0_SET_PICTURE_CHROMA_OFFSET0};
    for (uint32_t base : planes) {
        if (entry.method >= base && entry.method < base + 4 * slots && !(entry.method & 3))
            return (entry.method - base) / 4 == current ? SWITCH_NVDEC_WRITE : SWITCH_NVDEC_READ;
    }
    switch (entry.method) {
    case NVC5B0_SET_IN_BUF_BASE_OFFSET:
    case NVC5B0_SET_SLICE_OFFSETS_BUF_OFFSET:
    case NVC5B0_VP9_SET_PROB_TAB_BUF_OFFSET:
    case NVC5B0_VP9_SET_TILE_SIZE_BUF_OFFSET:
    case NVC5B0_VP9_SET_COL_MVREAD_BUF_OFFSET:
    case NVC5B0_VP9_SET_SEGMENT_READ_BUF_OFFSET:
    case NVC5B0_HEVC_SET_SCALING_LIST_OFFSET:
    case NVC5B0_HEVC_SET_TILE_SIZES_OFFSET:
        return SWITCH_NVDEC_READ;
    case NVC5B0_SET_NVDEC_STATUS_OFFSET:
    case NVC5B0_SET_DISPLAY_BUF_LUMA_OFFSET:
    case NVC5B0_SET_DISPLAY_BUF_CHROMA_OFFSET:
    case NVC5B0_VP9_SET_CTX_COUNTER_BUF_OFFSET:
    case NVC5B0_VP9_SET_COL_MVWRITE_BUF_OFFSET:
    case NVC5B0_VP9_SET_SEGMENT_WRITE_BUF_OFFSET:
        return SWITCH_NVDEC_WRITE;
    case NVC5B0_SET_COLOC_DATA_OFFSET:
    case NVC5B0_H264_SET_MBHIST_BUF_OFFSET:
    case NVC5B0_SET_HISTORY_OFFSET:
    case NVC5B0_VP9_SET_FILTER_BUFFER_OFFSET:
    case NVC5B0_HEVC_SET_FILTER_BUFFER_OFFSET:
    case NVC5B0_SET_PIC_SCRATCH_BUF_OFFSET:
        return SWITCH_NVDEC_READ | SWITCH_NVDEC_WRITE;
    default:
        return 0;
    }
}
