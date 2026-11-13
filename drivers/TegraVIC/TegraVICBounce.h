// SPDX-License-Identifier: BSD-2-Clause-Patent
// A surface VIC cannot address as it is (a source on CoreVideo's default 64-byte rows, since VIC reads source rows on
// multiples of 256; a plane off 256 bytes) goes through a scratch copy of itself that VIC can, made and copied back by
// the GPU's copy engine (TegraGM20BCopy.h): no CPU touches a pixel. The scratch's geometry is plain C, shared with the
// host test; SwitchVICBounceSubmit.cpp does the copies.
#ifndef TEGRA_VIC_BOUNCE_H
#define TEGRA_VIC_BOUNCE_H
#include <stdint.h>
#include "TegraVICRequest.h"

#define TEGRA_VIC_BOUNCE_PAGE         4096u
#define TEGRA_VIC_BOUNCE_OFFSET_ALIGN 16u  // the copy engine's plane offsets

static inline uint64_t tegra_vic_bounce_round(uint64_t v, uint64_t a)
{
    return (v + a - 1) / a * a;
}

// The bytes of a row of plane p that hold pixels.
static inline uint32_t tegra_vic_bounce_row_bytes(const TegraVICFormat *f, uint32_t width, uint32_t p)
{
    return tegra_vic_plane_width(f, width, p) * f->bpe[p];
}

// 1: VIC addresses every plane as it is (its pitch a multiple of `align`, tegra_vic_pitch_align's for the surface's
// side, and its offset of 256); 0: the surface needs the bounce; -1: it cannot have one either (a plane offset the copy
// engine cannot take, or a format VIC does not know).
static inline int tegra_vic_bounce_needed(const TegraVICSurfaceDesc *d, uint32_t align)
{
    const TegraVICFormat *f = tegra_vic_format(d->fourcc);
    if (!f)
        return -1;
    bool needed = false;
    for (uint32_t p = 0; p < f->planes; p++)
        needed = needed || d->planeBytesPerRow[p] % align != 0 || d->planeOffset[p] % 256 != 0;
    if (!needed)
        return 1;
    for (uint32_t p = 0; p < f->planes; p++) {
        if (d->planeOffset[p] % TEGRA_VIC_BOUNCE_OFFSET_ALIGN)
            return -1;
    }
    return 0;
}

// The scratch: the same pixels at pitches rounded up to `align`, its planes one after another on page boundaries.
static inline void tegra_vic_bounce_desc(const TegraVICSurfaceDesc *d, uint32_t align, TegraVICSurfaceDesc *out)
{
    const TegraVICFormat *f = tegra_vic_format(d->fourcc);
    *out = *d;
    uint64_t at = 0;
    for (uint32_t p = 0; p < f->planes; p++) {
        out->planeOffset[p] = at;
        out->planeBytesPerRow[p] = tegra_vic_bounce_round(tegra_vic_bounce_row_bytes(f, d->width, p), align);
        at = tegra_vic_bounce_round(at + out->planeBytesPerRow[p] * d->planeHeight[p], TEGRA_VIC_BOUNCE_PAGE);
    }
    out->length = at;
}

#endif
