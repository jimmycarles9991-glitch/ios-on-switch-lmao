// SPDX-License-Identifier: BSD-2-Clause-Patent
// What the kernel accepts in a TegraNVDECSubmit (TegraNVDECABI.h), shared with the host tests so a stream the plug-in
// builds is checked by the same code before it ever reaches hardware.
//
// NVDEC writes physical memory directly (no SMMU), so every size that steers its writes is checked here against the
// buffer the relocation points into, computed from the picture setup the kernel copies (the setup is what the engine
// obeys): for H.264 the decoded surfaces (pitch x 32-aligned height, NV12 block linear), the colocated-motion slots,
// the MB history, the history buffer and the status record; for VP9 the current surface (64-row superblocks), the
// segmentation map and colocated motion vectors it writes, the filter buffer (rows from its BSD control offset), the
// symbol counters and the status record; for HEVC all sixteen picture slots (pitch x 64-aligned height: whole CTUs),
// the seventeen colocated-motion slots, the filter buffer (deblocking, SAO and BSD control rows from the setup's
// offsets) and the status record. Reads (bitstreams, slice offsets, probability and tile size tables,
// reference surfaces, the previous map and vectors) are bounded by their buffers too. Every job must run with the
// engine's GP timer on and at most its default limit (1 << 27 cycles), which is what bounds a job whatever its
// bitstream holds (TegraNVDEC.cpp kJobBoundUs). Content protection, histogram and hint dumps (outputs to addresses no
// relocation names) are refused. MPEG-4 Part 2 and MPEG-1/2: TegraNVDECCheckMPEG.h.
#ifndef TEGRA_NVDEC_CHECK_H
#define TEGRA_NVDEC_CHECK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "TegraNVDECABI.h"
#include "nvidia/nvdec_drv.h"
#include "nvidia/clc5b0.h"

#ifdef __cplusplus
static_assert(sizeof(nvdec_h264_pic_s) <= TEGRA_NVDEC_MAX_SETUP, "H.264 setup travels inline");
static_assert(sizeof(nvdec_vp9_pic_s) <= TEGRA_NVDEC_MAX_SETUP, "VP9 setup travels inline");
static_assert(sizeof(nvdec_hevc_pic_s) <= TEGRA_NVDEC_MAX_SETUP, "HEVC setup travels inline");
#else
_Static_assert(sizeof(nvdec_h264_pic_s) <= TEGRA_NVDEC_MAX_SETUP, "H.264 setup travels inline");
_Static_assert(sizeof(nvdec_vp9_pic_s) <= TEGRA_NVDEC_MAX_SETUP, "VP9 setup travels inline");
_Static_assert(sizeof(nvdec_hevc_pic_s) <= TEGRA_NVDEC_MAX_SETUP, "HEVC setup travels inline");
#endif

#ifdef __cplusplus
extern "C" {
#endif

// The methods every codec's stream shares, written as values. NULL, or the reason; *seen collects them.
static inline const char *tegra_nvdec_check_value(const TegraNVDECSubmit *s, uint32_t i, uint32_t *seen)
{
    const TegraNVDECEntry *e = &s->entries[i];
    uint32_t bit = 0;
    switch (e->method) {
    case NVC5B0_SET_APPLICATION_ID:
        if (e->value != s->codec)
            return "application id";
        bit = 0;
        break;
    case NVC5B0_SET_CONTROL_PARAMS:  // codec type, GPTIMER_ON (required), RET_ERROR, ERR_CONCEAL_ON, MBTIMER_ON
        if (((e->value & 0xf) != s->codec && !(s->codec == TEGRA_NVDEC_CODEC_MPEG12 &&
                                               (e->value & 0xf) == NVC5B0_SET_CONTROL_PARAMS_CODEC_TYPE_MPEG1)) ||
            (e->value & ~0x207fu) != 0 || !(e->value & 0x10))
            return "control params";
        bit = 1;
        break;
    case NVC5B0_SET_PICTURE_INDEX:
        bit = 2;
        break;
    case NVC5B0_SET_WATCHDOG_TIMER:
        bit = 3;
        break;
    case NVC5B0_EXECUTE:  // AWAKEN only: NOTIFY would release a semaphore at a raw address
        if (e->value & ~0x100u)
            return "execute flags";
        if (i != s->entry_count - 1)
            return "execute not last";
        bit = 4;
        break;
    default:
        return "value method";
    }
    if (*seen & (1u << bit))
        return "repeated method";
    *seen |= 1u << bit;
    return NULL;  // NOLINT(modernize-use-nullptr): C too (avd_host.c)
}

#include "TegraNVDECCheckMPEG.h"  // needs tegra_nvdec_check_value

static inline const char *tegra_nvdec_check_h264(const TegraNVDECSubmit *s, const uint64_t *avail)
{
    enum {
        LUMA = NVC5B0_SET_PICTURE_LUMA_OFFSET0,
        CHROMA = NVC5B0_SET_PICTURE_CHROMA_OFFSET0,
        SLOTS = 17
    };

    if (s->setup_bytes != sizeof(nvdec_h264_pic_s))
        return "setup size";
    uint64_t bitstream = 0;
    uint64_t slices = 0;
    uint64_t status = 0;
    uint64_t coloc = 0;
    uint64_t mbhist = 0;
    uint64_t history = 0;
    uint64_t luma[SLOTS] = {0};
    uint64_t chroma[SLOTS] = {0};
    uint32_t seen = 0;
    uint32_t pictures = 0;
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVDECEntry *e = &s->entries[i];
        const uint16_t m = e->method;
        if (e->kind == TEGRA_NVDEC_ENTRY_VALUE) {
            const char *why = tegra_nvdec_check_value(s, i, &seen);
            if (why)
                return why;
            continue;
        }
        if (e->kind != TEGRA_NVDEC_ENTRY_RELOC)
            return "entry kind";
        if (e->offset & 0xff)
            return "relocation alignment";
        const uint64_t a = avail[i];
        uint64_t *slot = NULL;  // NOLINT(modernize-use-nullptr): C too (avd_host.c)
        switch (m) {
        case NVC5B0_SET_IN_BUF_BASE_OFFSET:
            slot = &bitstream;
            break;
        case NVC5B0_SET_SLICE_OFFSETS_BUF_OFFSET:
            slot = &slices;
            break;
        case NVC5B0_SET_NVDEC_STATUS_OFFSET:
            slot = &status;
            break;
        case NVC5B0_SET_COLOC_DATA_OFFSET:
            slot = &coloc;
            break;
        case NVC5B0_H264_SET_MBHIST_BUF_OFFSET:
            slot = &mbhist;
            break;
        case NVC5B0_SET_HISTORY_OFFSET:
            slot = &history;
            break;
        default:
            if (m >= LUMA && m < LUMA + 4 * SLOTS && !(m & 3)) {
                slot = &luma[(m - LUMA) / 4];
            } else if (m >= CHROMA && m < CHROMA + 4 * SLOTS && !(m & 3)) {
                slot = &chroma[(m - CHROMA) / 4];
            } else {
                return "relocated method";
            }
            pictures++;
        }
        if (*slot)
            return "repeated relocation";
        *slot = a ? a : 1;  // a relocation exactly at a buffer's end still counts as present
    }
    if ((seen & 0x13) != 0x13)
        return "missing application id, control params or execute";
    if (!bitstream || !slices || !status || !coloc || !mbhist || !history || pictures != 2 * SLOTS)
        return "missing relocation";

    const nvdec_h264_pic_s *p = (const nvdec_h264_pic_s *)s->setup;
    const uint8_t *otf = (const uint8_t *)&p->encryption_params;
    for (uint32_t i = 0; i < sizeof(p->encryption_params); i++) {
        if (otf[i])
            return "encryption parameters";
    }
    if (p->hint_dump_en)
        return "hint dump";
    if (!tegra_nvdec_zero(&p->displayPara, sizeof(p->displayPara)))
        return "display output or histogram";
    if (p->gptimer_timeout_value > (1u << 27))
        return "GP timer limit";  // 0: the firmware's 1 << 27
    const uint32_t w = (uint32_t)p->PicWidthInMbs;
    const uint32_t h = (uint32_t)p->FrameHeightInMbs;
    if (p->PicWidthInMbs < 3 || w > 256 || p->FrameHeightInMbs < 1 || h > 256)
        return "dimensions";
    // Interlaced: FrameHeightInMbs counts the frame's macroblock rows, two a map unit; a field picture or an MBAFF
    // frame writes its rows within the frame's surface, both fields of a frame into one (libnvtvmr's NV12 setups keep
    // every top/bottom/frame offset 0 and frame_surfaces clear for field pictures: 0x19770-0x197a4, 0x1a23c, 0x19850).
    const bool interlaced = !p->frame_mbs_only_flag;
    if ((p->field_pic_flag && !interlaced) || (p->MbaffFrameFlag && (!interlaced || p->field_pic_flag)) ||
        ((p->bottom_field_flag || p->second_field) && !p->field_pic_flag) || (interlaced && (h & 1)))
        return "picture structure";
    if (p->tileFormat || p->gob_height || p->output_memory_layout || p->frame_surfaces || p->chroma_format_idc != 1)
        return "surface format";
    if (p->luma_top_offset || p->luma_bot_offset || p->luma_frame_offset || p->chroma_top_offset ||
        p->chroma_bot_offset || p->chroma_frame_offset)
        return "surface offsets";
    if (p->pitch_luma != p->pitch_chroma || (p->pitch_luma & 63) || p->pitch_luma < w * 16 || p->pitch_luma > 16384)
        return "pitch";
    const uint64_t lumaBytes = (uint64_t)p->pitch_luma * ((h * 16 + 31) & ~31u);
    for (uint32_t i = 0; i < SLOTS; i++) {
        if (luma[i] < lumaBytes || chroma[i] < lumaBytes / 2)
            return "surface too small";
    }
    uint32_t cols = p->CurrColIdx;
    if (p->CurrPicIdx >= SLOTS || p->CurrColIdx >= SLOTS)
        return "picture index";
    for (uint32_t i = 0; i < 16; i++) {  // NOLINT(modernize-loop-convert): C too
        const nvdec_dpb_entry_s *e = &p->dpb[i];
        if (e->index >= SLOTS || e->col_idx >= SLOTS || e->output_memory_layout || (e->is_field && !interlaced) ||
            e->top_field_marking > 2 || e->bottom_field_marking > 2 || !e->top_field_marking != !(e->state & 1) ||
            !e->bottom_field_marking != !(e->state & 2))
            return "dpb entry";
        if (p->dpb[i].col_idx > cols)
            cols = p->dpb[i].col_idx;
    }
    const uint64_t colocBytes = ((((h + 1) & ~1u) * w * 64 - 63 + 255) & ~255u);
    if (coloc < colocBytes * (cols + 1))
        return "colocated buffer";
    if (p->mbhist_buffer_size < ((w * 104 + 255) & ~255u) || mbhist < p->mbhist_buffer_size)
        return "mb history";
    if ((uint64_t)p->HistBufferSize * 256 < ((w * 0x200 + 0x1100 + 0x1ff) & ~0x1ffu) ||
        history < (uint64_t)p->HistBufferSize * 256)
        return "history";
    if (status < sizeof(nvdec_status_s))
        return "status";
    if (!p->slice_count || slices < ((uint64_t)p->slice_count + 1) * 4 || !p->stream_len || bitstream < p->stream_len)
        return "bitstream";
    return NULL;  // NOLINT(modernize-use-nullptr): C too (avd_host.c)
}

// VP9 (profile 0, 8-bit 4:2:0): picture slots 0-2 are the LAST, GOLDEN and ALTREF references, 3 the current frame.
static inline const char *tegra_nvdec_check_vp9(const TegraNVDECSubmit *s, const uint64_t *avail)
{
    enum {
        LUMA = NVC5B0_SET_PICTURE_LUMA_OFFSET0,
        CHROMA = NVC5B0_SET_PICTURE_CHROMA_OFFSET0
    };

    enum {
        BITS,
        STATUS,
        PROBS,
        COUNTERS,
        TILES,
        MV_WRITE,
        MV_READ,
        SEG_READ,
        SEG_WRITE,
        FILTER,
        BUFFERS
    };

    if (s->setup_bytes != sizeof(nvdec_vp9_pic_s))
        return "setup size";
    uint64_t buf[BUFFERS] = {0};
    uint64_t luma[4] = {0};
    uint64_t chroma[4] = {0};
    uint32_t seen = 0;
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVDECEntry *e = &s->entries[i];
        const uint16_t m = e->method;
        if (e->kind == TEGRA_NVDEC_ENTRY_VALUE) {
            const char *why = tegra_nvdec_check_value(s, i, &seen);
            if (why)
                return why;
            continue;
        }
        if (e->kind != TEGRA_NVDEC_ENTRY_RELOC)
            return "entry kind";
        if (e->offset & 0xff)
            return "relocation alignment";
        uint64_t *slot = NULL;  // NOLINT(modernize-use-nullptr): C too (avd_host.c)
        switch (m) {
        case NVC5B0_SET_IN_BUF_BASE_OFFSET:
            slot = &buf[BITS];
            break;
        case NVC5B0_SET_NVDEC_STATUS_OFFSET:
            slot = &buf[STATUS];
            break;
        case NVC5B0_VP9_SET_PROB_TAB_BUF_OFFSET:
            slot = &buf[PROBS];
            break;
        case NVC5B0_VP9_SET_CTX_COUNTER_BUF_OFFSET:
            slot = &buf[COUNTERS];
            break;
        case NVC5B0_VP9_SET_TILE_SIZE_BUF_OFFSET:
            slot = &buf[TILES];
            break;
        case NVC5B0_VP9_SET_COL_MVWRITE_BUF_OFFSET:
            slot = &buf[MV_WRITE];
            break;
        case NVC5B0_VP9_SET_COL_MVREAD_BUF_OFFSET:
            slot = &buf[MV_READ];
            break;
        case NVC5B0_VP9_SET_SEGMENT_READ_BUF_OFFSET:
            slot = &buf[SEG_READ];
            break;
        case NVC5B0_VP9_SET_SEGMENT_WRITE_BUF_OFFSET:
            slot = &buf[SEG_WRITE];
            break;
        case NVC5B0_VP9_SET_FILTER_BUFFER_OFFSET:
            slot = &buf[FILTER];
            break;
        default:
            if (m >= LUMA && m < LUMA + 16 && !(m & 3)) {
                slot = &luma[(m - LUMA) / 4];
            } else if (m >= CHROMA && m < CHROMA + 16 && !(m & 3)) {
                slot = &chroma[(m - CHROMA) / 4];
            } else {
                return "relocated method";
            }
        }
        if (*slot)
            return "repeated relocation";
        *slot = avail[i] ? avail[i] : 1;  // a relocation exactly at a buffer's end still counts as present
    }
    if ((seen & 0x13) != 0x13)
        return "missing application id, control params or execute";
    for (uint32_t i = 0; i < BUFFERS; i++) {  // NOLINT(modernize-loop-convert): C too
        if (!buf[i])
            return "missing relocation";
    }
    for (uint32_t i = 0; i < 4; i++) {
        if (!luma[i] || !chroma[i])
            return "missing relocation";
    }

    const nvdec_vp9_pic_s *p = (const nvdec_vp9_pic_s *)s->setup;
    for (uint32_t i = 0; i < 4; i++) {
        if (p->wrapped_session_key[i] || p->wrapped_content_key[i] || p->initialization_vector[i])
            return "encryption parameters";
    }
    if (p->enable_encryption || p->key_increment || p->encryption_mode || p->sw_hdr_skip_length || p->key_slot_index ||
        p->ssm_en)
        return "encryption parameters";
    if (p->enable_histogram)
        return "histogram";
    const uint8_t *unused = (const uint8_t *)&p->v1;  // NVDEC 3+ only; T210 takes none of it
    for (uint32_t i = 0; i < sizeof(p->v1) + sizeof(p->ssm); i++) {
        if (unused[i])
            return "v1 fields";
    }
    if (p->gptimer_timeout_value > (1u << 27))
        return "GP timer limit";
    if (p->tileformat || p->gob_height || p->reserverd_surface_format)
        return "surface format";
    const uint32_t w = p->width;
    const uint32_t h = p->height;
    if (w < 8 || w > 4096 || h < 8 || h > 4096)
        return "dimensions";
    if (p->log2_tile_columns < 0 || p->log2_tile_columns > 6 || p->log2_tile_rows < 0 || p->log2_tile_rows > 2)
        return "tiles";
    // Surfaces: the current one is written in whole 64x64 superblocks; references are read within their own.
    const uint32_t dims[4][3] = {{p->ref0_width, p->ref0_height, p->ref0_stride[0]},
                                 {p->ref1_width, p->ref1_height, p->ref1_stride[0]},
                                 {p->ref2_width, p->ref2_height, p->ref2_stride[0]},
                                 {w, h, p->framestride[0]}};
    const uint32_t chromaStride[4] = {p->ref0_stride[1], p->ref1_stride[1], p->ref2_stride[1], p->framestride[1]};
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t rw = dims[i][0];
        const uint32_t rh = dims[i][1];
        const uint32_t pitch = dims[i][2];
        if (rw < 8 || rw > 4096 || rh < 8 || rh > 4096)
            return "reference dimensions";
        if (pitch != chromaStride[i] || (pitch & 63) || pitch < rw || pitch > 16384)
            return "pitch";
        const uint64_t lumaBytes = (uint64_t)pitch * ((rh + 63) & ~63u);
        if (luma[i] < lumaBytes || chroma[i] < lumaBytes / 2)
            return "surface too small";
    }
    const uint64_t sb = (uint64_t)((((w + 15) & ~15u) + 63) / 64) * ((((h + 15) & ~15u) + 63) / 64);
    if (buf[SEG_WRITE] < ((sb * 32 + 255) & ~255ULL) || buf[SEG_READ] < ((sb * 32 + 255) & ~255ULL))
        return "segmentation map";
    if (buf[MV_WRITE] < sb * 1024 || buf[MV_READ] < sb * 1024)
        return "motion vectors";
    if (buf[COUNTERS] < sizeof(nvdec_vp9EntropyCounts_t))
        return "counters";
    if (buf[PROBS] < sizeof(nvdec_vp9EntropyProbs_t))
        return "probabilities";
    if (buf[TILES] < 0x700)
        return "tile sizes";
    if (buf[STATUS] < sizeof(nvdec_status_s))
        return "status";
    // The filter buffer: filter rows (a multiple of 64, at least the frame's) x 912 bytes, then BSD control data at
    // Vp9BsdCtrlOffset (256-byte units), 988 bytes per row in all (averne's nvtegra_vp9.c layout).
    const uint64_t bsd = (uint64_t)p->Vp9BsdCtrlOffset * 256;
    if (bsd % (64ULL * 912))
        return "filter offset";
    const uint64_t rows = bsd / 912;
    if (rows < ((h + 63) & ~63u) || rows > 4096 || buf[FILTER] < rows * 988)
        return "filter buffer";
    if (!p->stream_len || buf[BITS] < p->stream_len)
        return "bitstream";
    return NULL;  // NOLINT(modernize-use-nullptr): C too (avd_host.c)
}

static inline const char *tegra_nvdec_check_hevc(const TegraNVDECSubmit *s, const uint64_t *avail)
{
    enum {
        LUMA = NVC5B0_SET_PICTURE_LUMA_OFFSET0,
        CHROMA = NVC5B0_SET_PICTURE_CHROMA_OFFSET0,
        SLOTS = 16
    };

    enum {
        BITS,
        STATUS,
        SCALING,
        TILES,
        FILTER,
        COLOC,
        BUFFERS,
        DISPLAY_LUMA = BUFFERS,
        DISPLAY_CHROMA,
        ALL_BUFFERS
    };

    if (s->setup_bytes != sizeof(nvdec_hevc_pic_s))
        return "setup size";
    uint64_t buf[ALL_BUFFERS] = {0};
    uint64_t luma[SLOTS] = {0};
    uint64_t chroma[SLOTS] = {0};
    uint32_t seen = 0;
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVDECEntry *e = &s->entries[i];
        const uint16_t m = e->method;
        if (e->kind == TEGRA_NVDEC_ENTRY_VALUE) {
            const char *why = tegra_nvdec_check_value(s, i, &seen);
            if (why)
                return why;
            continue;
        }
        if (e->kind != TEGRA_NVDEC_ENTRY_RELOC)
            return "entry kind";
        if (e->offset & 0xff)
            return "relocation alignment";
        uint64_t *slot = NULL;  // NOLINT(modernize-use-nullptr): C too (avd_host.c)
        switch (m) {
        case NVC5B0_SET_IN_BUF_BASE_OFFSET:
            slot = &buf[BITS];
            break;
        case NVC5B0_SET_NVDEC_STATUS_OFFSET:
            slot = &buf[STATUS];
            break;
        case NVC5B0_HEVC_SET_SCALING_LIST_OFFSET:
            slot = &buf[SCALING];
            break;
        case NVC5B0_HEVC_SET_TILE_SIZES_OFFSET:
            slot = &buf[TILES];
            break;
        case NVC5B0_HEVC_SET_FILTER_BUFFER_OFFSET:
            slot = &buf[FILTER];
            break;
        case NVC5B0_SET_COLOC_DATA_OFFSET:
            slot = &buf[COLOC];
            break;
        case NVC5B0_SET_DISPLAY_BUF_LUMA_OFFSET:
            slot = &buf[DISPLAY_LUMA];
            break;
        case NVC5B0_SET_DISPLAY_BUF_CHROMA_OFFSET:
            slot = &buf[DISPLAY_CHROMA];
            break;
        default:
            if (m >= LUMA && m < LUMA + 4 * SLOTS && !(m & 3)) {
                slot = &luma[(m - LUMA) / 4];
            } else if (m >= CHROMA && m < CHROMA + 4 * SLOTS && !(m & 3)) {
                slot = &chroma[(m - CHROMA) / 4];
            } else {
                return "relocated method";
            }
        }
        if (*slot)
            return "repeated relocation";
        *slot = avail[i] ? avail[i] : 1;
    }
    const bool display = buf[DISPLAY_LUMA] || buf[DISPLAY_CHROMA];
    if (display && (!buf[DISPLAY_LUMA] || !buf[DISPLAY_CHROMA]))
        return "half a display buffer";
    if ((seen & 0x13) != 0x13)
        return "missing application id, control params or execute";
    for (uint32_t i = 0; i < BUFFERS; i++) {  // NOLINT(modernize-loop-convert): C too
        if (!buf[i])
            return "missing relocation";
    }
    for (uint32_t i = 0; i < SLOTS; i++) {
        if (!luma[i] || !chroma[i])
            return "missing relocation";
    }

    const nvdec_hevc_pic_s *p = (const nvdec_hevc_pic_s *)s->setup;
    for (uint32_t i = 0; i < 4; i++) {
        if (p->wrapped_session_key[i] || p->wrapped_content_key[i] || p->initialization_vector[i])
            return "encryption parameters";
    }
    if (p->enable_encryption || p->key_increment || p->encryption_mode || p->key_slot_index || p->ssm_en)
        return "encryption parameters";
    if (p->enable_histogram || p->enable_substream_decoding)
        return "histogram or substream decoding";
    const uint8_t *unused = (const uint8_t *)&p->ecdma_cfg;  // error-concealment DMA, then NVDEC 3+ fields
    for (uint32_t i = 0; i < sizeof(p->ecdma_cfg); i++) {
        if (unused[i])
            return "ecdma";
    }
    unused = (const uint8_t *)&p->v1;
    for (uint32_t i = 0; i < sizeof(p->v1) + sizeof(p->v2) + sizeof(p->v3) + sizeof(p->ssm); i++) {
        if (unused[i])
            return "v1-v3 fields";
    }
    if (p->gptimer_timeout_value > (1u << 27))
        return "GP timer limit";
    if (p->tileformat || p->gob_height || p->reserverd_surface_format)
        return "surface format";
    const uint32_t w = p->pic_width_in_luma_samples;
    const uint32_t h = p->pic_height_in_luma_samples;
    if (w < 144 || w > 4096 || h < 144 || h > 4096 || (w & 7) || (h & 7))
        return "dimensions";
    if (p->chroma_format_idc != 1 || p->bit_depth_luma != p->bit_depth_chroma)
        return "chroma format";
    // Mode 0 writes 8-bit samples; mode 1, Main 10's only, writes 16-bit ones, so its surfaces and the pixel rows of
    // the filter buffer are twice as wide. Modes 2 and 3 take Main 10 to 8 bits (nvdec_drv.h: Rec.709, Rec.2020), into
    // the pictures (averne's nvtegra_hevc.c, without a display buffer) or into the display buffer (NVDEC's
    // DISPLAY_BUF, which no open driver binds); which, and how wide, is unknown, so both are bounded as 16-bit.
    const bool eightBit = p->bit_depth_luma == 8 && p->disp_output_mode == 0;
    const bool tenBit = p->bit_depth_luma == 10 && p->disp_output_mode >= 1 && p->disp_output_mode <= 3;
    if (!eightBit && !tenBit)
        return "output mode";
    if (display && p->disp_output_mode < 2)
        return "display buffer outside modes 2 and 3";
    const uint64_t sampleBytes = tenBit ? 2 : 1;
    if (p->log2_min_luma_coding_block_size < 3 || p->log2_max_luma_coding_block_size > 6 ||
        p->log2_min_luma_coding_block_size > p->log2_max_luma_coding_block_size ||
        p->log2_max_luma_coding_block_size < 4 || p->log2_min_transform_block_size < 2 ||
        p->log2_max_transform_block_size > 5)
        return "block sizes";
    if (p->num_tile_columns > 20 || p->num_tile_rows > 22 || p->curr_pic_idx >= SLOTS)
        return "tiles or slot";
    // Surfaces: the current one is written in whole CTUs (64-aligned rows); references are read within their own.
    // framestride counts samples; the engine's rows are framestride x sampleBytes bytes, in whole 64-byte GOBs.
    const uint32_t stride = p->framestride[0];
    const uint64_t pitch = (uint64_t)stride * sampleBytes;
    if (stride != p->framestride[1] || (pitch & 63) || stride < ((w + 63) & ~63u) || pitch > 16384)
        return "pitch";
    const uint64_t rows = (h + 63) & ~63u;
    const uint64_t lumaBytes = pitch * rows;
    for (uint32_t i = 0; i < SLOTS; i++) {
        if (luma[i] < lumaBytes || chroma[i] < lumaBytes / 2)
            return "surface too small";
    }
    // SET_DISPLAY_BUF_SIZE stays refused ("value method"): its unit is undocumented, so no bound on it can be proven.
    if (display && (buf[DISPLAY_LUMA] < lumaBytes || buf[DISPLAY_CHROMA] < lumaBytes / 2))
        return "display buffer too small";
    // Colocated motion: 17 slots of colMvBuffersize x 256 bytes, each at least (64-aligned width x height) / 16.
    const uint64_t colmv = (uint64_t)p->colMvBuffersize * 256;
    if (colmv < ((w + 63) & ~63u) * rows / 16 || buf[COLOC] < 17 * colmv)
        return "colocated motion";
    // The filter buffer: 480 bytes of deblocking per row, SAO (3840 per row) from HevcSaoBufferOffset and BSD control
    // (60 per row) from HevcBsdCtrlOffset, both in 256-byte units, over at least the 64-aligned height; deblocking and
    // SAO per sample byte.
    const uint64_t sao = (uint64_t)p->HevcSaoBufferOffset * 256;
    const uint64_t bsd = (uint64_t)p->HevcBsdCtrlOffset * 256;
    if (sao < 480 * sampleBytes * rows || bsd < sao + 3840 * sampleBytes * rows || buf[FILTER] < bsd + 60 * rows)
        return "filter buffer";
    if (buf[TILES] < 0x900)
        return "tile sizes";
    if (buf[SCALING] < sizeof(nvdec_hevc_scaling_list_s))
        return "scaling lists";
    if (buf[STATUS] < sizeof(nvdec_status_s))
        return "status";
    if (!p->stream_len || buf[BITS] < p->stream_len)
        return "bitstream";
    return NULL;  // NOLINT(modernize-use-nullptr): C too (avd_host.c)
}

// avail[i]: bytes from entry i's relocated address to the end of its buffer (ignored for values).
// Returns NULL when the submit is acceptable, else the reason.
static inline const char *tegra_nvdec_check(const TegraNVDECSubmit *s, const uint64_t *avail)
{
    if (s->entry_count < 1 || s->entry_count > TEGRA_NVDEC_MAX_ENTRIES || s->clean_count > TEGRA_NVDEC_MAX_RANGES)
        return "counts";
    switch (s->codec) {
    case TEGRA_NVDEC_CODEC_H264:
        return tegra_nvdec_check_h264(s, avail);
    case TEGRA_NVDEC_CODEC_VP9:
        return tegra_nvdec_check_vp9(s, avail);
    case TEGRA_NVDEC_CODEC_HEVC:
        return tegra_nvdec_check_hevc(s, avail);
    case TEGRA_NVDEC_CODEC_MPEG4:
        return tegra_nvdec_check_mpeg4(s, avail);
    case TEGRA_NVDEC_CODEC_MPEG12:
        return tegra_nvdec_check_mpeg2(s, avail);
    default:
        return "codec";
    }
}

#ifdef __cplusplus
}
#endif

#endif
