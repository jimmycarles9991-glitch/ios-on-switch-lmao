// SPDX-License-Identifier: BSD-2-Clause-Patent
// A submit's input picture from an IOSurface (TegraNVENCABI.h TegraNVENCSource): VIC writes the surface's region into
// the buffer IN_CUR_PIC names, exactly where and how input_cfg says the firmware reads it, after the connection's last
// job (which may still read the buffer) on the hardware. The submit was checked first, so the planes VIC writes are
// the ones the checker bounded.
#include <stddef.h>
#include "TegraNVENC.h"
#include "TegraNVENCCheck.h"
#include "../TegraVIC/SwitchVICEncodeInput.h"

static_assert(offsetof(nvenc_h264_drv_pic_setup_s, input_cfg) == offsetof(nvenc_h265_drv_pic_setup_s, input_cfg) &&
                  sizeof(nvenc_h264_surface_cfg_s) == sizeof(nvenc_h265_surface_cfg_s),
              "both apps read input_cfg at one place in one layout");

namespace {
const TegraNVENCEntry *relocation(const TegraNVENCSubmit *s, uint16_t method)
{
    for (uint32_t i = 0; i < s->entry_count; i++) {
        if (s->entries[i].method == method && s->entries[i].kind == TEGRA_NVENC_ENTRY_RELOC)
            return &s->entries[i];
    }
    return nullptr;
}

uint32_t log2Of(uint32_t gobs)
{
    uint32_t log2 = 0;
    while ((2u << log2) <= gobs)
        log2++;
    return log2;
}
}  // namespace

bool tegra_nvenc_source_ready(void)
{
    return switch_vic_encode_input_ready(true);
}

uint32_t tegra_nvenc_source_row_align(void)
{
    return tegra_vic_pitch_align(tegra_vic_traits(), true);
}

int tegra_nvenc_source(const TegraNVENCSubmit *s, TegraNVENCResolve resolve, void *context, task_t task,
                       const uint32_t *last, TegraNVENCAfter *after, uint32_t *route, const char **why)
{
    const TegraNVENCSource &source = s->source;
    const TegraNVENCEntry *luma = relocation(s, NVC5B7_SET_IN_CUR_PIC);
    const TegraNVENCEntry *chroma = relocation(s, NVC5B7_SET_IN_CUR_PIC_CHROMA_U);
    if (!luma || !chroma || luma->value != chroma->value) {
        *why = "a source surface needs the input's planes in one buffer";
        return -22;
    }
    TegraNVENCBlock block = {};
    if (!resolve(context, luma->value, &block) || !block.backing) {
        *why = "a source surface's input buffer";
        return -22;
    }
    nvenc_h264_surface_cfg_s cfg;
    memcpy(&cfg, s->setup + offsetof(nvenc_h264_drv_pic_setup_s, input_cfg), sizeof(cfg));
    if (cfg.tiled_16x16) {
        *why = "a source surface into 16x16 tiles, which VIC does not write";
        return -95;
    }
    const uint32_t width = cfg.frame_width_minus1 + 1u, height = cfg.frame_height_minus1 + 1u;
    if (!source.width || !source.height || source.width > width || source.height > height) {
        *why = "a source region larger than the picture";
        return -22;
    }
    const uint32_t blockRows = tegra_nvenc_block_rows(false, cfg.block_height);
    const TegraVICAcquire previous = {tegra_nvenc_syncpt(), last ? *last : 0};
    SwitchVICEncodeInput input = {};
    input.destination = block.backing;
    input.lumaOffset = luma->offset + uint64_t(cfg.luma_top_frm_offset) * 256;
    input.chromaOffset = chroma->offset + uint64_t(cfg.chroma_top_frm_offset) * 256;
    input.pitch = cfg.sfc_pitch;
    input.lumaRows = tegra_nvenc_luma_rows(height, blockRows);
    input.chromaRows = tegra_nvenc_chroma_rows(height, blockRows);
    input.blockLinear = cfg.block_height != 0;
    input.blockHeightLog2 = cfg.block_height ? log2Of(cfg.block_height) : 0;
    input.width = width;
    input.height = height;
    input.x = source.x;
    input.y = source.y;
    input.w = source.width;
    input.h = source.height;
    input.fourcc = source.format;
    input.matrix = source.matrix;
    input.after = last ? &previous : nullptr;
    uint32_t fence = 0;
    switch_gpu_backing_token written = {};
    bool bounced = false;
    const int error = switch_vic_encode_input(source.surface_id, task, &input, &fence, &written, &bounced, why);
    if (!error) {
        *after = {tegra_vic_syncpt(), fence, written};
        *route = bounced ? TEGRA_NVENC_INPUT_BOUNCED : TEGRA_NVENC_INPUT_VIC;
    }
    return error;
}
