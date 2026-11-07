// SPDX-License-Identifier: BSD-2-Clause-Patent
// The rows of an IOSurface plane a rectangle covers (a "band"), for jobs that write part of a surface: a picture placed
// at an offset (a grid image's tile) maps only those rows into VIC's window and cleans only those lines. Plain C over
// the surface's geometry, shared with the host test.
#ifndef TEGRA_VIC_BAND_H
#define TEGRA_VIC_BAND_H
#include <stdint.h>

#define TEGRA_VIC_BAND_PAGE 4096u

typedef struct {
    uint64_t start;  // the rectangle's first byte in this plane, from the surface's first byte
    uint32_t pitch, rows, rowBytes;
} TegraVICBand;

// Plane `plane` of a 4:2:0 biplanar surface ('420v', '420f'): the rectangle (x, y, w, h) in luma pixels, all even.
// A CbCr row holds w bytes (w / 2 pairs) from byte x.
static inline TegraVICBand tegra_vic_band_420(uint64_t planeOffset, uint32_t pitch, uint32_t plane, uint32_t x,
                                              uint32_t y, uint32_t w, uint32_t h)
{
    TegraVICBand b;
    const uint32_t row = plane ? y / 2 : y;
    b.start = planeOffset + (uint64_t)row * pitch + x;
    b.pitch = pitch;
    b.rows = plane ? h / 2 : h;
    b.rowBytes = w;
    return b;
}

static inline uint64_t tegra_vic_band_end(const TegraVICBand *b)
{
    return b->rows ? b->start + (uint64_t)(b->rows - 1) * b->pitch + b->rowBytes : b->start;
}

// The 4 KiB pages the band touches, from the one holding its first byte.
static inline uint32_t tegra_vic_band_pages(const TegraVICBand *b)
{
    const uint64_t first = b->start / TEGRA_VIC_BAND_PAGE;
    const uint64_t last = (tegra_vic_band_end(b) + TEGRA_VIC_BAND_PAGE - 1) / TEGRA_VIC_BAND_PAGE;
    return (uint32_t)(last - first);
}

// Each row's bytes as runs inside single pages: fn(context, page index from the band's first page, offset in that
// page, length). Lines a run shares with bytes outside the rectangle are the caller's to judge.
typedef void (*TegraVICBandRun)(void *context, uint32_t page, uint32_t offset, uint32_t length);

static inline void tegra_vic_band_runs(const TegraVICBand *b, TegraVICBandRun fn, void *context)
{
    const uint64_t base = b->start / TEGRA_VIC_BAND_PAGE * TEGRA_VIC_BAND_PAGE;
    for (uint32_t r = 0; r < b->rows; r++) {
        uint64_t at = b->start + (uint64_t)r * b->pitch - base;
        uint64_t left = b->rowBytes;
        while (left) {
            const uint32_t offset = (uint32_t)(at % TEGRA_VIC_BAND_PAGE);
            const uint32_t length =
                (uint32_t)(left < TEGRA_VIC_BAND_PAGE - offset ? left : TEGRA_VIC_BAND_PAGE - offset);
            fn(context, (uint32_t)(at / TEGRA_VIC_BAND_PAGE), offset, length);
            at += length;
            left -= length;
        }
    }
}

#endif
