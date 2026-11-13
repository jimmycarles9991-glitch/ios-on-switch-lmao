// SPDX-License-Identifier: BSD-2-Clause-Patent
// Whether NVENC reads a decoder's native picture as it is (TegraNVENCDirect.cpp), and the input_cfg that says how: the
// picture's planes are 16Bx2 block linear with 2^n GOBs a block, and block_height 2^n (n 1..5) is a layout NVENC 5.0
// reads (codes 0..4, TegraNVENCCheckCommon.h; NVDEC writes n = 1, the code NVIDIA's driver feeds NVENC). Plain C:
// the kernel and the host tests share it, and the checker then bounds the patched submit against the picture's backing.
#ifndef TEGRA_NVENC_DIRECT_PLAN_H
#define TEGRA_NVENC_DIRECT_PLAN_H
#include "TegraNVENCCheckCommon.h"
#include "../TegraGM20B/SwitchGpuBacking.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t pitch, blockHeight;
    uint64_t luma, chroma;  // plane offsets in the picture's backing
} TegraNVENCDirectPlan;

// `cfg`: the submit's input_cfg; `source`: its TegraNVENCSource; `layout` and the crop: the native picture's. NULL and
// the plan, or why the picture goes through VIC instead.
static inline const char *tegra_nvenc_direct_plan(const nvenc_h264_surface_cfg_s *cfg, const TegraNVENCSource *source,
                                                  const struct switch_gpu_backing_layout *layout, uint32_t cropX,
                                                  uint32_t cropY, uint32_t cropWidth, uint32_t cropHeight,
                                                  TegraNVENCDirectPlan *plan)
{
    const struct switch_gpu_backing_plane *luma = &layout->planes[0], *chroma = &layout->planes[1];
    if (layout->plane_count != 2 || layout->format != source->format ||
        (source->format != 0x34323076u && source->format != 0x34323066u))
        return "native picture of another format";
    if (luma->bytes_per_element != 1 || chroma->bytes_per_element != 2)
        return "native picture not 8-bit NV12";
    if (luma->layout != SWITCH_GPU_BACKING_BLOCK_LINEAR || chroma->layout != SWITCH_GPU_BACKING_BLOCK_LINEAR ||
        luma->block_height_log2 != chroma->block_height_log2 || luma->block_height_log2 < 1 ||
        luma->block_height_log2 > 5)
        return "native block height NVENC has no code for";
    if (luma->pitch != chroma->pitch || (luma->pitch & 63) || luma->pitch > 8176)
        return "native pitch NVENC does not address";
    if ((luma->offset | chroma->offset) & 0xff)
        return "native plane off 256 bytes";
    if (cropX || cropY || source->x || source->y || cropWidth != source->width || cropHeight != source->height)
        return "native picture cropped, or a region of it";
    if (cfg->tiled_16x16)
        return "a session reading 16x16 tiles";
    plan->pitch = luma->pitch;
    plan->blockHeight = 1u << luma->block_height_log2;
    plan->luma = luma->offset;
    plan->chroma = chroma->offset;
    return NULL;  // NOLINT(modernize-use-nullptr): C too (host tests)
}

// The plan into the submit's input_cfg: the picture's pitch and block height, chroma found by its own relocation.
static inline void tegra_nvenc_direct_config(const TegraNVENCDirectPlan *plan, nvenc_h264_surface_cfg_s *cfg)
{
    cfg->sfc_pitch = cfg->sfc_pitch_chroma = (unsigned short)plan->pitch;
    cfg->block_height = plan->blockHeight;
    cfg->tiled_16x16 = 0;
    cfg->luma_top_frm_offset = cfg->chroma_top_frm_offset = 0;
}

// What a job reading the picture does about one of its producers, `status` that engine's (1 done, 0 still writing,
// negative failed): 0 nothing (done, or an NVENC job, ordered before it on NVENC's channel); 1 wait on the hardware for
// syncpoint `queue` to reach the low 32 bits of `value` (NVDEC's and VIC's); -11 when only the GPU's bridge can order
// the producer, so the picture goes through VIC; the failure itself, which fails the frame as VIC's route does.
static inline int tegra_nvenc_direct_producer(const struct switch_gpu_backing_token *producer, int status)
{
    if (status < 0)
        return status;
    if (status == 1 || producer->engine == SWITCH_GPU_ENGINE_NVENC)
        return 0;
    if ((producer->engine != SWITCH_GPU_ENGINE_NVDEC && producer->engine != SWITCH_GPU_ENGINE_VIC) ||
        producer->queue > 0xff)
        return -11;
    return 1;
}

#ifdef __cplusplus
}
#endif
#endif
