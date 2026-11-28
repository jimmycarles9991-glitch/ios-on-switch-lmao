// SPDX-License-Identifier: BSD-2-Clause-Patent
// How a planned decode reaches its destination once GM20B has said how it laid the destination out. Plain C, header
// only: TegraNVJPGJobs.cpp and the host test (test/request_host.c) compile the same code.
#ifndef TEGRA_NVJPG_ROUTE_H
#define TEGRA_NVJPG_ROUTE_H
#include <stdbool.h>
#include <stdint.h>
#include "TegraNVJPGRequest.h"
#include "../TegraGM20B/TegraGM20BLayout.h"

#ifdef __cplusplus
extern "C" {
#endif

// What GM20B answered for the destination's first page: `held` the hold's tiled answer (the surface decided
// block-linear), `described` whether tegra_gm20b_surface_layout gave `layout`, `compressed` a compressible kind.
typedef struct {
    bool held, described, compressed;
    struct tgm_layout layout;
} TegraNVJPGDestinationLayout;

enum {
    TEGRA_NVJPG_ROUTE_ROWS = 0,         // NVJPG writes the destination's rows
    TEGRA_NVJPG_ROUTE_COPY_ROWS = 1,    // NVJPG writes scratch rows, the copy engine the destination's rows
    TEGRA_NVJPG_ROUTE_COPY_BLOCKS = 2,  // NVJPG writes scratch rows, the copy engine the destination's blocks
    TEGRA_NVJPG_ROUTE_TILE_PROBE = 3,   // debug kernels: NVJPG told the tiling HW Test's probe asks for
    TEGRA_NVJPG_ROUTE_COUNT = 4,
};

// The copy engine writes a block-linear side only whole (TegraGM20BCopy.cpp's layout): the plan's one copy must be the
// layout's every texel, from the destination's first byte.
static inline bool tegra_nvjpg_copy_covers_blocks(const TegraNVJPGPlan *p, const struct tgm_layout *l)
{
    return p->planes == 1 && !p->fillChroma && p->copyDstStart[0] == 0 &&
           (uint64_t)l->width * l->bpp == p->copyRowBytes[0] && l->height == p->copyRows[0];
}

// `p` (as tegra_nvjpg_plan left it, a single-surface plan: not VIC's re-pack) and its setup's output fields moved onto
// the destination's layout `l`. NVJPG writes rows only (TegraNVJPGJpeg.h), so a destination GM20B laid out
// block-linear takes them through the copy engine, which writes its blocks. `maxRepack` the copy engine's scratch,
// `copyEngine` whether the copy engine can reach the destination's memory. NULL with *route, or why the destination's
// layout cannot be written.
static inline const char *tegra_nvjpg_route(const TegraNVJPGDecodeRequest *r, const TegraNVJPGSurface *d,
                                            uint64_t maxRepack, bool copyEngine, const TegraNVJPGDestinationLayout *l,
                                            TegraNVJPGPlan *p, nvjpg_dec_drv_pic_setup_s *s, uint32_t *route)
{
    if (p->repack == TEGRA_NVJPG_REPACK_VIC)
        return "VIC's re-pack has no single destination layout to route";
    if (!l->held) {
        *route = p->repack == TEGRA_NVJPG_REPACK_CE ? TEGRA_NVJPG_ROUTE_COPY_ROWS : TEGRA_NVJPG_ROUTE_ROWS;
        return NULL;
    }
    if (!l->described)
        return "destination decided block-linear, but GM20B describes no layout of it";
    if (l->compressed)
        return "compressed block-linear destination";
    *route = TEGRA_NVJPG_ROUTE_COPY_BLOCKS;
    if (!copyEngine)
        return "block-linear destination the copy engine cannot reach";
    if (p->repack == TEGRA_NVJPG_REPACK_CE)
        return tegra_nvjpg_copy_covers_blocks(p, &l->layout)
                   ? NULL
                   : "copy-engine re-pack into part of a block-linear destination";
    return tegra_nvjpg_plan_fallback_ce(r, d, maxRepack, true, l->layout.width, l->layout.height, l->layout.bpp, false,
                                        p, s)
               ? NULL
               : "a picture that is not the whole of its block-linear destination, or past the copy engine's scratch";
}

// HW Test image.nvjpg_tile_probe's decode (debug kernels): the picture whole into the blocks GM20B laid the destination
// out in, from its first byte, with `tileMode` and `gob` written to the setup's tile_mode and block_linear_height
// unchanged, which the T210's firmware passes to PDMA_PIC_INFO1. Told tile_mode 2 the engine wrote rows
// (TegraNVJPGJpeg.h); this settles every mode and block height on the device. The code's GOBs a block are 2^gob, or
// 2^(gob + 1) if the decoder takes the encoder's gobHeight coding (nvjpg_drv.h): the block rows the larger reaches lie
// inside the destination. Where tile mode 1 puts its bytes is unproven: its stride may count 64-byte GOB columns, not
// bytes, and its blocks run to 2^8 GOBs. The plan's writeEnd covers every byte those readings reach, so the mapping's
// sink takes a misread write instead of the output window's next mapping, another surface. `window` the most one
// mapping may span. NULL with *route, or why not.
static inline const char *tegra_nvjpg_route_tile_probe(const TegraNVJPGDecodeRequest *r, uint64_t length,
                                                       uint64_t window, const TegraNVJPGDestinationLayout *l,
                                                       uint32_t tileMode, uint32_t gob, TegraNVJPGPlan *p,
                                                       nvjpg_dec_drv_pic_setup_s *s, uint32_t *route)
{
    if (!l->held || !l->described || l->compressed)
        return "a tile probe into a destination GM20B did not lay out block-linear, uncompressed";
    if (p->repack != TEGRA_NVJPG_REPACK_NONE || p->planes != 1 || p->fillChroma || r->x_offset || r->y_offset ||
        tileMode > 3 || gob > 7)
        return "a tile probe of more than one plane, placed, or of a tiling the setup has no field for";
    const uint32_t told = gob + 1 > l->layout.y_log2 ? gob + 1 : l->layout.y_log2;
    const uint64_t blockRows = 8ull << told;
    const uint64_t end = (p->extent.rows[0] + blockRows - 1) / blockRows * blockRows * l->layout.row_bytes;
    if (p->extent.rowBytes[0] > l->layout.row_bytes || end > length)
        return "a tile probe whose block rows run past the destination";
    const uint64_t worst =
        (uint64_t)p->extent.rows[0] * l->layout.row_bytes * 64 + (uint64_t)l->layout.row_bytes * (8u << 8);
    if (worst > window)
        return "a tile probe whose worst reading of its tiling runs past one mapping's window";
    p->target.pitch[0] = l->layout.row_bytes;
    p->planeStart[0] = 0;
    p->writeEnd = end > worst ? end : worst;
    s->output_stride_luma = (int)l->layout.row_bytes;
    s->tile_mode = (int)tileMode;
    s->block_linear_height = (int)gob;
    *route = TEGRA_NVJPG_ROUTE_TILE_PROBE;
    return NULL;
}

#ifdef __cplusplus
}
#endif
#endif
