// SPDX-License-Identifier: BSD-2-Clause-Patent
// A surface whose rows VIC cannot address (CoreVideo's default 64-byte rows; VIC takes multiples of 256) goes through a
// scratch copy of itself at a pitch VIC takes, made and copied back by the GPU's copy engine (TegraGM20BCopy.h): no CPU
// touches a pixel. The scratch's geometry is plain C, shared with the host test; TegraVICBounce.cpp does the copies.
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
    return (f->subsampled && p ? width / 2 : width) * f->bpe[p];
}

// 1: every plane's pitch is one VIC addresses; 0: the surface needs the bounce; -1: it cannot have one either (a plane
// offset the copy engine cannot take, or a format VIC does not know).
static inline int tegra_vic_bounce_needed(const TegraVICSurfaceDesc *d, uint32_t align)
{
    const TegraVICFormat *f = tegra_vic_format(d->fourcc);
    if (!f)
        return -1;
    int needed = 0;
    for (uint32_t p = 0; p < f->planes; p++)
        needed |= d->planeBytesPerRow[p] % align != 0;
    if (!needed)
        return 1;
    for (uint32_t p = 0; p < f->planes; p++)
        if (d->planeOffset[p] % TEGRA_VIC_BOUNCE_OFFSET_ALIGN)
            return -1;
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

// The copy engine raises its syncpoint this much after each copy that signals (nvgpu's release: two increments).
#define TEGRA_VIC_BOUNCE_INCREMENTS 2u

// Where a job's copies and VIC meet, on the engines: the fills (a source into its scratch, then a destination's own
// pixels into its scratch) run in order on the copy engine and only the last signals; VIC's channel waits for that
// value; a destination's copy back waits for VIC's fence and signals the value the job is finished at. `next` is the
// copy engine's syncpoint value after everything queued so far, advanced past this job's signals.
typedef struct {
    uint32_t fills;
    int vicWaits, copiesBack;
    uint32_t vicAcquire;  // the copy-engine value VIC's channel waits for
    uint32_t done;        // the copy-engine value the job is finished at, after its copy back
} TegraVICBounceOrder;

static inline TegraVICBounceOrder tegra_vic_bounce_order(int bounceSrc, int bounceDst, uint32_t *next)
{
    TegraVICBounceOrder o = {0, 0, 0, 0, 0};
    o.fills = (bounceSrc ? 1u : 0u) + (bounceDst ? 1u : 0u);
    o.vicWaits = o.fills != 0;
    o.copiesBack = bounceDst != 0;
    if (o.vicWaits)
        o.vicAcquire = *next += TEGRA_VIC_BOUNCE_INCREMENTS;
    if (o.copiesBack)
        o.done = *next += TEGRA_VIC_BOUNCE_INCREMENTS;
    return o;
}

#endif
