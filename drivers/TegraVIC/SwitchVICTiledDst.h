// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include "SwitchVICOverwrite.h"
#include "TegraVICConfig.h"
#include "TegraVICRequest.h"
#include "TegraVICSurface.h"
#include "../TegraGM20B/SwitchGpuBacking.h"
#include "../TegraGM20B/TegraGM20BLayout.h"

struct SwitchVICTiledDst {
    uint32_t pitch, rows, blockHeightLog2;
    uint64_t bytes;
};

// An uncompressed single-plane GM20B block-linear surface as VIC addresses it (GENERIC_16Bx2 GOBs, the block height the
// GPU chose), from its backing's layout.
static inline bool switch_vic_tiled_layout(const TegraVICSurface *surface, switch_gpu_backing *backing,
                                           uint64_t accessOffset, SwitchVICTiledDst *out)
{
    if (surface->baseOffset || accessOffset)
        return false;
    const TegraVICFormat *f = tegra_vic_format(surface->desc.fourcc);
    if (!f || f->planes != 1 || f->packed)
        return false;
    switch_gpu_backing_info info = {};
    switch_gpu_backing_describe(backing, &info);
    if (!info.page_count || !info.pages || tegra_gm20b_surface_compressed(info.pages[0]) != 0)
        return false;
    uint32_t pitch = 0, rows = 0, yLog2 = 0;
    uint64_t bytes = 0;
    if (info.layout.plane_count) {
        if (info.layout.plane_count != 1)
            return false;
        const auto &p = info.layout.planes[0];
        if (p.layout != SWITCH_GPU_BACKING_BLOCK_LINEAR || p.offset || p.width != surface->desc.width ||
            p.height != surface->desc.height || p.bytes_per_element != f->bpe[0] || p.block_height_log2 > 5)
            return false;
        pitch = p.pitch;
        yLog2 = p.block_height_log2;
        const uint32_t blockRows = 8u << yLog2;
        rows = (p.height + blockRows - 1) / blockRows * blockRows;
        bytes = p.bytes;
    } else {
        tgm_layout tiled = {};
        if (tegra_gm20b_surface_layout(info.pages[0], &tiled) != 1)
            return false;
        if (tiled.width != surface->desc.width || tiled.height != surface->desc.height || tiled.bpp != f->bpe[0] ||
            tiled.y_log2 > 5)
            return false;
        pitch = tiled.row_bytes;
        rows = tiled.rows;
        yLog2 = tiled.y_log2;
        bytes = tiled.bytes;
    }
    if (!pitch || (pitch & 63u) || (pitch % f->bpe[0]) || !rows || (rows % (8u << yLog2)) ||
        rows < surface->desc.height)
        return false;
    const uint64_t planeBytes = static_cast<uint64_t>(pitch) * rows;
    const uint64_t backingBytes = static_cast<uint64_t>(info.page_count) * 4096;
    if (planeBytes > bytes || bytes > backingBytes)
        return false;
    *out = {pitch, rows, yLog2, bytes};
    return true;
}

// Uncompressed single-plane GM20B block-linear destination VIC can write directly (OutBlkKind GENERIC_16Bx2).
// Requires an untransposed whole-surface overwrite so whole-block writes cannot disturb untouched sub-rectangles.
static inline bool switch_vic_tiled_dst(const TegraVICSurface *destination, switch_gpu_backing *backing,
                                        uint64_t accessOffset, const TegraVICPlan *plan, const TegraVICTraits *traits,
                                        SwitchVICTiledDst *out)
{
    if (!traits || !traits->blockLinearOut || !destination || !backing || !plan || !out)
        return false;
    for (uint32_t i = 0; i < plan->count; ++i)
        if (plan->op[i].transform & TEGRA_VIC_ROT90)
            return false;
    if (!switch_vic_overwrites(*plan, destination->desc))
        return false;
    return switch_vic_tiled_layout(destination, backing, accessOffset, out);
}

// An RGB source the GPU rendered block-linear, read by VIC where it lies instead of untiled into a pitch scratch by the
// copy engine first (a quarter turn of a detached Metal layer: one job a present during Maps, 3.6 MB untiled each).
static inline bool switch_vic_tiled_src(const TegraVICSurface *source, switch_gpu_backing *backing,
                                        uint64_t accessOffset, const TegraVICTraits *traits, SwitchVICTiledDst *out)
{
    if (!traits || !traits->blockLinearRgbIn || !source || !backing || !out)
        return false;
    const TegraVICFormat *f = tegra_vic_format(source->desc.fourcc);
    if (!f || f->kind != TEGRA_VIC_KIND_RGB)
        return false;
    return switch_vic_tiled_layout(source, backing, accessOffset, out);
}

static inline const char *switch_vic_tiled_image(const TegraVICSurface *surface, const SwitchVICTiledDst &tiled,
                                                 uint64_t mappingAddress, TegraVICImage *out)
{
    const char *why = tegra_vic_image_from(&surface->desc, mappingAddress, out);
    if (why)
        return why;
    out->blockLinear = 1;
    out->blockHeightLog2 = tiled.blockHeightLog2;
    out->pitch[0] = tiled.pitch;
    out->rows[0] = tiled.rows;
    out->base[0] = mappingAddress;
    out->span[0] = tiled.bytes;
    return nullptr;
}
