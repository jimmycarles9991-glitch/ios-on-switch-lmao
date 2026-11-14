// SPDX-License-Identifier: BSD-2-Clause-Patent
// A model of what VIC 4.0 writes for a job TegraVICConfig.h builds, for the host tests (test:tegra_vic_transpose_test):
// the output layout as measured on the hardware, not NVIDIA's documentation (there is none for it). It reads the job's
// configuration and methods as the engine does, composes slot 0 onto the target rectangle and writes the result:
//   rectangles  the target rectangle, the slot's destination rectangle and the output surface size are in the composed
//               picture's coordinates, before any transpose (on the unit, a 16x32 target given for a 32x16 card was
//               filled 2:1 across and 1:2 down, then turned)
//   memory      composed pixel (x, y) goes to row y, column x of a plane whose rows are LumaWidth + 1 elements apart or,
//               transposed, to row x, column y with rows LumaHeight + 1 elements apart (LumaWidth + 1 rows): hekate's
//               270-degree job (OutLumaWidth 1279, OutLumaHeight 719 into a 720-pixel-stride framebuffer) and the unit's
//               128-byte row stride from LumaHeight 31 into 256-byte rows; chroma the same way, halved along memory's
//               axes whether or not the job transposes: N420 a pair for each 2x2 written pixels, N422 for each two
//               across a memory row, N422R for each two down a memory column (probe gpu.x1.vic_transpose422, boots 19
//               and 21: a transposed N422 output was exact, a transposed N422R one misplaced every pair). A pair is
//               written at its even memory row and column, sited across the row by OutChromaLocHoriz (0 the even
//               sample's chroma, 1 the mean of both, 2 the odd one's: the same probe's offsets), never down (the even
//               row's); writes outside the surface size or the luma geometry are dropped (counted as clipped)
//   flips       mirror where each composed pixel is written, within the target rectangle before the transpose
//               (VIC_MODEL_FLIP_RECT), or, the other models V4's probes must cope with, around the whole surface
//               (VIC_MODEL_FLIP_SURFACE: a sub-rectangle lands at its mirror image) or after the transpose
//               (VIC_MODEL_FLIP_AFTER): where VIC's flips act is measured on the unit, not assumed
//   sampling    composed pixel centre x + 0.5 reads source column floor(left + (x + 0.5) * srcW / dstW) (nearest); a
//               2-tap job is modelled as 1-tap (the same at 1:1)
//   colour      10-bit pipeline: RGB in as x << 2, Y'CbCr in as x << 2 | x >> 6 (V4's measured widening), the slot and
//               output matrices through tegra_vic_matrix_apply, out as x >> 2; Cb in pipeline channel 2, Cr in 1
//               (T210B01, as V4 measures it)
// Every access is checked against the regions the test maps; one outside them counts as a fault. Pitch linear only.
#ifndef VIC_MODEL_H
#define VIC_MODEL_H
#include <stdint.h>
#include <string.h>
#include "../TegraVICConfig.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    VIC_MODEL_FLIP_RECT = 0,
    VIC_MODEL_FLIP_SURFACE = 1,
    VIC_MODEL_FLIP_AFTER = 2
};

typedef struct {
    uint64_t iova;
    uint8_t *va;
    uint64_t bytes;
} VicModelRegion;

typedef struct {
    VicModelRegion region[4];
    uint32_t regions;
    uint32_t flips;                    // VIC_MODEL_FLIP_*
    uint64_t writes, clipped, faults;  // pixels (and CbCr pairs) written, dropped, accesses outside every region
} VicModel;

static inline uint8_t *vic_model_at(VicModel *m, uint64_t iova, uint32_t bytes)
{
    for (uint32_t i = 0; i < m->regions; i++) {
        const VicModelRegion *r = &m->region[i];
        if (iova >= r->iova && iova + bytes <= r->iova + r->bytes)
            return r->va + (iova - r->iova);
    }
    m->faults++;
    return 0;
}

static inline uint32_t vic_model_method(const TegraVICJob *j, uint32_t method, int *found)
{
    for (uint32_t i = 0; i < j->count; i++)
        if (j->method[i] == method) {
            *found = 1;
            return j->value[i];
        }
    *found = 0;
    return 0;
}

// A MatrixStruct as written: 20-bit signed coefficients, the shift, the enable bit.
static inline int vic_model_matrix(const uint8_t *cfg, uint32_t base, TegraVICMatrix *m)
{
    static const uint32_t at[3][4] = {{MATRIX_C00_BIT, MATRIX_C01_BIT, MATRIX_C02_BIT, MATRIX_C03_BIT},
                                      {MATRIX_C10_BIT, MATRIX_C11_BIT, MATRIX_C12_BIT, MATRIX_C13_BIT},
                                      {MATRIX_C20_BIT, MATRIX_C21_BIT, MATRIX_C22_BIT, MATRIX_C23_BIT}};
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 4; k++) {
            const uint32_t v = (uint32_t)tegra_vic_get(cfg, base, at[r][k], 20);
            m->c[r][k] = (int32_t)(v ^ 0x80000u) - 0x80000;
        }
    m->shift = (uint32_t)TEGRA_VIC_GET(cfg, base, MATRIX_SHIFT);
    return (int)TEGRA_VIC_GET(cfg, base, MATRIX_ENABLE);
}

typedef struct {
    uint32_t c[3];  // pipeline channels, 10-bit
    uint32_t a;     // alpha, 10-bit
} VicModelPixel;

static inline uint32_t vic_model_widen(uint32_t v8)
{
    return (v8 << 2) | (v8 >> 6);
}

typedef struct {
    uint32_t format, pitched, lw, lh, cw, matrixOn;
    uint64_t luma, chroma;
    TegraVICMatrix matrix;
} VicModelSlot;

static inline void vic_model_slot(const uint8_t *cfg, uint64_t luma, uint64_t chroma, VicModelSlot *s)
{
    const uint32_t sf = TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_SURFACE;
    s->format = (uint32_t)TEGRA_VIC_GET(cfg, sf, SURF_FORMAT);
    s->pitched = TEGRA_VIC_GET(cfg, sf, SURF_BLK_KIND) == TEGRA_VIC_BLK_PITCH;
    s->lw = (uint32_t)TEGRA_VIC_GET(cfg, sf, SURF_LUMA_WIDTH) + 1;
    s->lh = (uint32_t)TEGRA_VIC_GET(cfg, sf, SURF_LUMA_HEIGHT) + 1;
    s->cw = (uint32_t)TEGRA_VIC_GET(cfg, sf, SURF_CHROMA_WIDTH) + 1;
    s->matrixOn = (uint32_t)vic_model_matrix(cfg, TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_MATRIX, &s->matrix);
    s->luma = luma;
    s->chroma = chroma;
}

static inline int vic_model_read(VicModel *m, const VicModelSlot *s, uint32_t x, uint32_t y, VicModelPixel *p)
{
    if (!s->pitched || x >= s->lw || y >= s->lh) {
        m->faults++;
        return 0;
    }
    if (s->format == TEGRA_VIC_T_A8R8G8B8 || s->format == TEGRA_VIC_T_A8B8G8R8) {
        const uint8_t *q = vic_model_at(m, s->luma + ((uint64_t)y * s->lw + x) * 4, 4);
        if (!q)
            return 0;
        uint32_t w;
        memcpy(&w, q, 4);
        const uint32_t r = s->format == TEGRA_VIC_T_A8R8G8B8 ? (w >> 16) & 0xff : w & 0xff;
        const uint32_t b = s->format == TEGRA_VIC_T_A8R8G8B8 ? w & 0xff : (w >> 16) & 0xff;
        p->c[0] = r << 2;
        p->c[1] = ((w >> 8) & 0xff) << 2;
        p->c[2] = b << 2;
        p->a = vic_model_widen(w >> 24);
    } else if (s->format == TEGRA_VIC_T_Y8___U8V8_N420 || s->format == TEGRA_VIC_T_Y8___U8V8_N422) {
        const uint32_t cy = s->format == TEGRA_VIC_T_Y8___U8V8_N420 ? y / 2 : y;
        const uint8_t *yq = vic_model_at(m, s->luma + (uint64_t)y * s->lw + x, 1);
        const uint8_t *cq = vic_model_at(m, s->chroma + ((uint64_t)cy * s->cw + x / 2) * 2, 2);
        if (!yq || !cq)
            return 0;
        p->c[0] = vic_model_widen(yq[0]);
        p->c[2] = vic_model_widen(cq[0]);  // Cb
        p->c[1] = vic_model_widen(cq[1]);  // Cr
        p->a = 1023;
    } else {
        m->faults++;
        return 0;
    }
    if (s->matrixOn) {
        const uint32_t in10[3] = {p->c[0], p->c[1], p->c[2]};
        tegra_vic_matrix_apply(&s->matrix, in10, p->c);
    }
    return 1;
}

// Source coordinate (16.16 edges `lo`..`hi`, inclusive of the last pixel) of destination pixel `d` of `lo2`..`hi2`.
static inline uint32_t vic_model_sample(uint64_t lo, uint64_t hi, uint32_t lo2, uint32_t hi2, uint32_t d)
{
    const uint64_t srcW = hi - lo + 65536, dstW = (uint64_t)hi2 - lo2 + 1;
    return (uint32_t)((lo + ((2 * (uint64_t)(d - lo2) + 1) * srcW) / (2 * dstW)) >> 16);
}

typedef struct {
    uint32_t L, R, T, B, sw, sh, fx, fy, inSurface;
    int slot, alphaFill, outMatrixOn;
    uint64_t SL, SR, ST, SB;
    uint32_t DL, DR, DT, DB;
    VicModelPixel background;
    TegraVICMatrix outMatrix;
    VicModelSlot in;
} VicModelJob;

// The composed pixel at (x, y) of the target rectangle, after the output matrix; 0 if its source read faulted.
static inline int vic_model_pixel(VicModel *m, const VicModelJob *j, uint32_t x, uint32_t y, VicModelPixel *p)
{
    *p = j->background;
    if (j->slot && x >= j->DL && x <= j->DR && y >= j->DT && y <= j->DB) {
        const uint32_t sx = vic_model_sample(j->SL, j->SR, j->DL, j->DR, x);
        const uint32_t sy = vic_model_sample(j->ST, j->SB, j->DT, j->DB, y);
        if (!vic_model_read(m, &j->in, sx, sy, p))
            return 0;
        if (j->alphaFill == TEGRA_VIC_ALPHA_BACKGROUND)
            p->a = j->background.a;
        else if (j->alphaFill != TEGRA_VIC_ALPHA_SOURCE_STREAM)
            p->a = 1023;
    }
    if (j->outMatrixOn) {
        const uint32_t in10[3] = {p->c[0], p->c[1], p->c[2]};
        tegra_vic_matrix_apply(&j->outMatrix, in10, p->c);
    }
    return 1;
}

// A flip mirrors within the target rectangle [lo, hi] or around the surface of `size`; it is its own inverse.
static inline uint32_t vic_model_mirror(const VicModelJob *j, uint32_t flip, uint32_t size, uint32_t lo, uint32_t hi,
                                        uint32_t v)
{
    if (!flip)
        return v;
    return j->inSurface ? size - 1 - v : lo + hi - v;
}

// The chroma a pair written at memory (row, col) and (row, col + 1) carries under OutChromaLocHoriz `site`: the even
// sample's (0), the mean of both (1) or the odd one's (2). A partner outside the target rectangle leaves the even
// one's.
static inline void vic_model_site(VicModel *m, const VicModelJob *j, uint32_t tr, uint32_t site, uint32_t row,
                                  uint32_t col, VicModelPixel *p)
{
    if (!site)
        return;
    const uint32_t wx = tr ? row : col + 1, wy = tr ? col + 1 : row;
    const uint32_t x = vic_model_mirror(j, j->fx, j->sw, j->L, j->R, wx);
    const uint32_t y = vic_model_mirror(j, j->fy, j->sh, j->T, j->B, wy);
    VicModelPixel odd;
    if (x < j->L || x > j->R || y < j->T || y > j->B || !vic_model_pixel(m, j, x, y, &odd))
        return;
    for (int c = 1; c < 3; c++)
        p->c[c] = site == 1 ? (p->c[c] + odd.c[c]) / 2 : odd.c[c];
}

static inline void vic_model_run(VicModel *m, const TegraVICJob *job)
{
    const uint8_t *c = job->config;
    int found = 0;
    const uint64_t outLuma = (uint64_t)vic_model_method(job, TEGRA_VIC_M_OUTPUT_LUMA, &found) << 8;
    const uint64_t outChroma = (uint64_t)vic_model_method(job, TEGRA_VIC_M_OUTPUT_CHROMA_U, &found) << 8;
    const uint64_t inLuma = (uint64_t)vic_model_method(job, TEGRA_VIC_M_SURFACE0_SLOT0_LUMA, &found) << 8;
    const uint64_t inChroma = (uint64_t)vic_model_method(job, TEGRA_VIC_M_SURFACE0_SLOT0_CHROMA_U, &found) << 8;
    const uint32_t tr = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_TRANSPOSE);
    VicModelJob j;
    j.fx = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_FLIP_X);
    j.fy = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_FLIP_Y);
    if (m->flips == VIC_MODEL_FLIP_AFTER && tr) {  // a flip of memory's x after the transpose is the composed y's
        const uint32_t k = j.fx;
        j.fx = j.fy;
        j.fy = k;
    }
    j.inSurface = m->flips == VIC_MODEL_FLIP_SURFACE;
    j.L = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_LEFT);
    j.R = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_RIGHT);
    j.T = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_TOP);
    j.B = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_BOTTOM);
    const uint32_t os = TEGRA_VIC_OUTPUT_SURFACE;
    const uint32_t format = (uint32_t)TEGRA_VIC_GET(c, os, SURF_FORMAT);
    const uint32_t site = (uint32_t)TEGRA_VIC_GET(c, os, SURF_CHROMA_LOC_HORIZ);
    j.sw = (uint32_t)TEGRA_VIC_GET(c, os, SURF_WIDTH) + 1;
    j.sh = (uint32_t)TEGRA_VIC_GET(c, os, SURF_HEIGHT) + 1;
    const uint32_t lw = (uint32_t)TEGRA_VIC_GET(c, os, SURF_LUMA_WIDTH) + 1;
    const uint32_t lh = (uint32_t)TEGRA_VIC_GET(c, os, SURF_LUMA_HEIGHT) + 1;
    const uint32_t cw = (uint32_t)TEGRA_VIC_GET(c, os, SURF_CHROMA_WIDTH) + 1;
    const uint32_t ch = (uint32_t)TEGRA_VIC_GET(c, os, SURF_CHROMA_HEIGHT) + 1;
    j.slot = (int)TEGRA_VIC_GET(c, TEGRA_VIC_SLOTS, SLOT_ENABLE);
    j.SL = TEGRA_VIC_GET(c, TEGRA_VIC_SLOTS, SLOT_SOURCE_LEFT);
    j.SR = TEGRA_VIC_GET(c, TEGRA_VIC_SLOTS, SLOT_SOURCE_RIGHT);
    j.ST = TEGRA_VIC_GET(c, TEGRA_VIC_SLOTS, SLOT_SOURCE_TOP);
    j.SB = TEGRA_VIC_GET(c, TEGRA_VIC_SLOTS, SLOT_SOURCE_BOTTOM);
    j.DL = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_SLOTS, SLOT_DEST_LEFT);
    j.DR = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_SLOTS, SLOT_DEST_RIGHT);
    j.DT = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_SLOTS, SLOT_DEST_TOP);
    j.DB = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_SLOTS, SLOT_DEST_BOTTOM);
    j.alphaFill = (int)TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_ALPHA_FILL_MODE);
    j.background.c[0] = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_R);
    j.background.c[1] = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_G);
    j.background.c[2] = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_B);
    j.background.a = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_ALPHA);
    j.outMatrixOn = vic_model_matrix(c, TEGRA_VIC_OUT_MATRIX, &j.outMatrix);
    vic_model_slot(c, inLuma, inChroma, &j.in);
    for (uint32_t y = j.T; y <= j.B; y++)
        for (uint32_t x = j.L; x <= j.R; x++) {
            VicModelPixel p;
            if (!vic_model_pixel(m, &j, x, y, &p))
                continue;
            const uint32_t wx = vic_model_mirror(&j, j.fx, j.sw, j.L, j.R, x);
            const uint32_t wy = vic_model_mirror(&j, j.fy, j.sh, j.T, j.B, y);
            if (wx >= j.sw || wy >= j.sh || wx >= lw || wy >= lh) {
                m->clipped++;
                continue;
            }
            // Memory: row wy, column wx; transposed, row wx, column wy.
            const uint32_t row = tr ? wx : wy, col = tr ? wy : wx;
            const uint64_t at = (uint64_t)row * (tr ? lh : lw) + col;
            if (format == TEGRA_VIC_T_A8R8G8B8 || format == TEGRA_VIC_T_A8B8G8R8) {
                uint8_t *q = vic_model_at(m, outLuma + at * 4, 4);
                if (!q)
                    continue;
                const uint32_t r = p.c[0] >> 2, g = p.c[1] >> 2, b = p.c[2] >> 2, a = p.a >> 2;
                const uint32_t w =
                    a << 24 | (format == TEGRA_VIC_T_A8R8G8B8 ? (r << 16 | g << 8 | b) : (b << 16 | g << 8 | r));
                memcpy(q, &w, 4);
                m->writes++;
            } else if (format == TEGRA_VIC_T_Y8___U8V8_N420 || format == TEGRA_VIC_T_Y8___U8V8_N422 ||
                       format == TEGRA_VIC_T_Y8___U8V8_N422R) {
                uint8_t *q = vic_model_at(m, outLuma + at, 1);
                if (!q)
                    continue;
                q[0] = (uint8_t)(p.c[0] >> 2);
                m->writes++;
                const uint32_t halfCol = format != TEGRA_VIC_T_Y8___U8V8_N422R;
                const uint32_t halfRow = format != TEGRA_VIC_T_Y8___U8V8_N422;
                if ((halfCol && (col & 1)) || (halfRow && (row & 1)))
                    continue;
                const uint32_t ccol = halfCol ? col / 2 : col, crow = halfRow ? row / 2 : row;
                const uint32_t cstride = tr ? ch : cw, crows = tr ? cw : ch;
                if (ccol >= cstride || crow >= crows) {
                    m->clipped++;
                    continue;
                }
                if (halfCol)
                    vic_model_site(m, &j, tr, site, row, col, &p);
                uint8_t *cq = vic_model_at(m, outChroma + ((uint64_t)crow * cstride + ccol) * 2, 2);
                if (!cq)
                    continue;
                cq[0] = (uint8_t)(p.c[2] >> 2);  // Cb
                cq[1] = (uint8_t)(p.c[1] >> 2);  // Cr
                m->writes++;
            } else {
                m->faults++;
            }
        }
}

#ifdef __cplusplus
}
#endif
#endif
