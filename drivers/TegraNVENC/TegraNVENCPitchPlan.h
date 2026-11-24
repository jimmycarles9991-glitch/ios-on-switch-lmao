// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef TEGRA_NVENC_PITCH_PLAN_H
#define TEGRA_NVENC_PITCH_PLAN_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "TegraNVENCCheckCommon.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TEGRA_NVENC_PITCH_HANDLE = 0xfffffffeu,
};

typedef struct TegraNVENCPitchPlan {
    uint32_t inputHandle;
    uint32_t width, height;
    uint32_t srcPitch;
    uint64_t srcLumaOffset, srcChromaOffset;
    uint32_t bl2Pitch, bl2LumaRows, bl2ChromaRows;
    uint64_t bl2ChromaOffset, scratchBytes;
} TegraNVENCPitchPlan;

static inline bool tegra_nvenc_is_pitch_input(const TegraNVENCSubmit *s)
{
    const size_t offset = offsetof(nvenc_h264_drv_pic_setup_s, input_cfg);
    if (!s || s->setup_bytes < offset + sizeof(nvenc_h264_surface_cfg_s))
        return false;
    nvenc_h264_surface_cfg_s cfg;
    memcpy(&cfg, s->setup + offset, sizeof(cfg));
    return !cfg.tiled_16x16 && cfg.block_height == 0;
}

static inline const char *tegra_nvenc_pitch_plan(const TegraNVENCSubmit *s, TegraNVENCPitchPlan *plan)
{
    if (!s || !plan)
        return "arguments";
    memset(plan, 0, sizeof(*plan));
    if (s->entry_count > TEGRA_NVENC_MAX_ENTRIES || s->clean_count > TEGRA_NVENC_MAX_RANGES)
        return "counts";
    if (!tegra_nvenc_is_pitch_input(s))
        return "input is not pitch linear";
    const TegraNVENCEntry *luma = NULL;    // NOLINT(modernize-use-nullptr): C too (host tests)
    const TegraNVENCEntry *chroma = NULL;  // NOLINT(modernize-use-nullptr): C too (host tests)
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVENCEntry *e = &s->entries[i];
        if (e->kind != TEGRA_NVENC_ENTRY_RELOC)
            continue;
        if (e->method == NVC5B7_SET_IN_CUR_PIC)
            luma = e;
        else if (e->method == NVC5B7_SET_IN_CUR_PIC_CHROMA_U)
            chroma = e;
    }
    if (!luma || !chroma || luma->value != chroma->value)
        return "pitch input requires luma and chroma in one buffer";
    nvenc_h264_surface_cfg_s cfg;
    memcpy(&cfg, s->setup + offsetof(nvenc_h264_drv_pic_setup_s, input_cfg), sizeof(cfg));
    if (cfg.sfc_trans_mode || cfg.reserved2 || cfg.memory_mode || cfg.nv21_enable || cfg.input_bl_mode || cfg.reserved)
        return "pitch input surface mode unsupported";
    if (cfg.luma_bot_offset || cfg.chroma_bot_offset)
        return "pitch input field offsets unsupported";
    const uint32_t width = cfg.frame_width_minus1 + 1u;
    const uint32_t height = cfg.frame_height_minus1 + 1u;
    if (!width || !height || (width & 1u) || (height & 1u) || width > TEGRA_NVENC_MAX_DIMENSION ||
        height > TEGRA_NVENC_MAX_DIMENSION)
        return "pitch input dimensions unsupported";
    if (cfg.sfc_pitch != cfg.sfc_pitch_chroma || (cfg.sfc_pitch & 63u) ||
        cfg.sfc_pitch < tegra_nvenc_align(width, 16) || cfg.sfc_pitch > 8176)
        return "pitch input stride unsupported";
    const uint64_t lumaOff = (uint64_t)luma->offset + (uint64_t)cfg.luma_top_frm_offset * 256u;
    const uint64_t chromaOff = (uint64_t)chroma->offset + (uint64_t)cfg.chroma_top_frm_offset * 256u;
    const uint64_t lumaBytes = (uint64_t)cfg.sfc_pitch * height;
    const uint64_t chromaBytes = (uint64_t)cfg.sfc_pitch * (height / 2u);
    if (lumaOff > UINT64_MAX - lumaBytes || chromaOff > UINT64_MAX - chromaBytes)
        return "pitch input plane offset overflow";
    if (lumaOff + lumaBytes > chromaOff && chromaOff + chromaBytes > lumaOff)
        return "pitch input planes overlap";
    const uint32_t bl2Pitch = tegra_nvenc_align(width, 64);
    if (bl2Pitch > 8176)
        return "BL2 scratch pitch exceeds NVENC limit";
    const uint32_t bl2LumaRows = tegra_nvenc_luma_rows(height, 16);
    const uint32_t bl2ChromaRows = tegra_nvenc_chroma_rows(height, 16);
    const uint64_t bl2ChromaOff = (uint64_t)bl2Pitch * bl2LumaRows;
    const uint64_t bl2ChromaBytes = (uint64_t)bl2Pitch * bl2ChromaRows;
    if (bl2ChromaOff > UINT32_MAX || bl2ChromaOff > UINT64_MAX - bl2ChromaBytes)
        return "BL2 scratch size overflow";
    plan->inputHandle = luma->value;
    plan->width = width;
    plan->height = height;
    plan->srcPitch = cfg.sfc_pitch;
    plan->srcLumaOffset = lumaOff;
    plan->srcChromaOffset = chromaOff;
    plan->bl2Pitch = bl2Pitch;
    plan->bl2LumaRows = bl2LumaRows;
    plan->bl2ChromaRows = bl2ChromaRows;
    plan->bl2ChromaOffset = bl2ChromaOff;
    plan->scratchBytes = bl2ChromaOff + bl2ChromaBytes;
    return NULL;  // NOLINT(modernize-use-nullptr): C too (host tests)
}

static inline void tegra_nvenc_pitch_patch(const TegraNVENCSubmit *s, const TegraNVENCPitchPlan *plan,
                                           uint32_t scratchHandle, TegraNVENCSubmit *patched)
{
    *patched = *s;
    nvenc_h264_surface_cfg_s cfg;
    const size_t offset = offsetof(nvenc_h264_drv_pic_setup_s, input_cfg);
    memcpy(&cfg, patched->setup + offset, sizeof(cfg));
    cfg.sfc_pitch = cfg.sfc_pitch_chroma = (unsigned short)plan->bl2Pitch;
    cfg.block_height = 2;
    cfg.tiled_16x16 = 0;
    cfg.luma_top_frm_offset = 0;
    cfg.chroma_top_frm_offset = 0;
    memcpy(patched->setup + offset, &cfg, sizeof(cfg));
    const uint32_t entryCount =
        patched->entry_count <= TEGRA_NVENC_MAX_ENTRIES ? patched->entry_count : TEGRA_NVENC_MAX_ENTRIES;
    bool inputReferenced = false;
    for (uint32_t i = 0; i < entryCount; i++) {
        TegraNVENCEntry *e = &patched->entries[i];
        if (e->kind != TEGRA_NVENC_ENTRY_RELOC)
            continue;
        if (e->method == NVC5B7_SET_IN_CUR_PIC) {
            e->value = scratchHandle;
            e->offset = 0;
        } else if (e->method == NVC5B7_SET_IN_CUR_PIC_CHROMA_U) {
            e->value = scratchHandle;
            e->offset = (uint32_t)plan->bl2ChromaOffset;
        } else if (e->value == plan->inputHandle) {
            inputReferenced = true;
        }
    }
    const uint32_t cleanCount =
        patched->clean_count <= TEGRA_NVENC_MAX_RANGES ? patched->clean_count : TEGRA_NVENC_MAX_RANGES;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < cleanCount; i++) {
        if (!inputReferenced && patched->clean[i].handle == plan->inputHandle)
            continue;
        patched->clean[kept++] = patched->clean[i];
    }
    patched->clean_count = kept;
}

static inline void tegra_nvenc_source_bl2_patch(const TegraNVENCSubmit *s, TegraNVENCSubmit *patched)
{
    *patched = *s;
    if (!tegra_nvenc_is_pitch_input(patched))
        return;
    nvenc_h264_surface_cfg_s cfg;
    const size_t offset = offsetof(nvenc_h264_drv_pic_setup_s, input_cfg);
    memcpy(&cfg, patched->setup + offset, sizeof(cfg));
    cfg.block_height = 2;
    memcpy(patched->setup + offset, &cfg, sizeof(cfg));
}

#ifdef __cplusplus
}
#endif
#endif
