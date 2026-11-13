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
//               128-byte row stride from LumaHeight 31 into 256-byte rows; 4:2:0 chroma the same way, one CbCr pair for
//               each 2x2 written pixels (from the top-left one); writes outside the surface size or the luma geometry
//               are dropped (counted as clipped). N422 and N422R chroma are taken to follow the same law: a pair for
//               each 2x1 across or 1x2 down the composed picture, so a transposed N422R plane is 4:2:2 in memory (an
//               inference from the 4:2:0 measurements, not itself measured)
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

static inline void vic_model_run(VicModel *m, const TegraVICJob *job)
{
    const uint8_t *c = job->config;
    int found = 0;
    const uint64_t outLuma = (uint64_t)vic_model_method(job, TEGRA_VIC_M_OUTPUT_LUMA, &found) << 8;
    const uint64_t outChroma = (uint64_t)vic_model_method(job, TEGRA_VIC_M_OUTPUT_CHROMA_U, &found) << 8;
    const uint64_t inLuma = (uint64_t)vic_model_method(job, TEGRA_VIC_M_SURFACE0_SLOT0_LUMA, &found) << 8;
    const uint64_t inChroma = (uint64_t)vic_model_method(job, TEGRA_VIC_M_SURFACE0_SLOT0_CHROMA_U, &found) << 8;
    const uint32_t tr = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_TRANSPOSE);
    uint32_t fx = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_FLIP_X);
    uint32_t fy = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_FLIP_Y);
    if (m->flips == VIC_MODEL_FLIP_AFTER && tr) {  // a flip of memory's x after the transpose is the composed y's
        const uint32_t k = fx;
        fx = fy;
        fy = k;
    }
    const uint32_t L = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_LEFT);
    const uint32_t R = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_RIGHT);
    const uint32_t T = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_TOP);
    const uint32_t B = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_BOTTOM);
    const uint32_t os = TEGRA_VIC_OUTPUT_SURFACE;
    const uint32_t format = (uint32_t)TEGRA_VIC_GET(c, os, SURF_FORMAT);
    const uint32_t sw = (uint32_t)TEGRA_VIC_GET(c, os, SURF_WIDTH) + 1,
                   sh = (uint32_t)TEGRA_VIC_GET(c, os, SURF_HEIGHT) + 1;
    const uint32_t lw = (uint32_t)TEGRA_VIC_GET(c, os, SURF_LUMA_WIDTH) + 1;
    const uint32_t lh = (uint32_t)TEGRA_VIC_GET(c, os, SURF_LUMA_HEIGHT) + 1;
    const uint32_t cw = (uint32_t)TEGRA_VIC_GET(c, os, SURF_CHROMA_WIDTH) + 1;
    const uint32_t ch = (uint32_t)TEGRA_VIC_GET(c, os, SURF_CHROMA_HEIGHT) + 1;
    const int slot = (int)TEGRA_VIC_GET(c, TEGRA_VIC_SLOTS, SLOT_ENABLE);
    const uint64_t SL = TEGRA_VIC_GET(c, TEGRA_VIC_SLOTS, SLOT_SOURCE_LEFT);
    const uint64_t SR = TEGRA_VIC_GET(c, TEGRA_VIC_SLOTS, SLOT_SOURCE_RIGHT);
    const uint64_t ST = TEGRA_VIC_GET(c, TEGRA_VIC_SLOTS, SLOT_SOURCE_TOP);
    const uint64_t SB = TEGRA_VIC_GET(c, TEGRA_VIC_SLOTS, SLOT_SOURCE_BOTTOM);
    const uint32_t DL = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_SLOTS, SLOT_DEST_LEFT);
    const uint32_t DR = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_SLOTS, SLOT_DEST_RIGHT);
    const uint32_t DT = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_SLOTS, SLOT_DEST_TOP);
    const uint32_t DB = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_SLOTS, SLOT_DEST_BOTTOM);
    const int sourceAlpha = TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_ALPHA_FILL_MODE) == TEGRA_VIC_ALPHA_SOURCE_STREAM;
    VicModelPixel background;
    background.c[0] = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_R);
    background.c[1] = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_G);
    background.c[2] = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_B);
    background.a = (uint32_t)TEGRA_VIC_GET(c, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_ALPHA);
    TegraVICMatrix outMatrix;
    const int outMatrixOn = vic_model_matrix(c, TEGRA_VIC_OUT_MATRIX, &outMatrix);
    VicModelSlot in;
    vic_model_slot(c, inLuma, inChroma, &in);
    for (uint32_t y = T; y <= B; y++)
        for (uint32_t x = L; x <= R; x++) {
            VicModelPixel p = background;
            if (slot && x >= DL && x <= DR && y >= DT && y <= DB) {
                const uint32_t sx = vic_model_sample(SL, SR, DL, DR, x), sy = vic_model_sample(ST, SB, DT, DB, y);
                if (!vic_model_read(m, &in, sx, sy, &p))
                    continue;
                if (!sourceAlpha)
                    p.a = 1023;
            }
            if (outMatrixOn) {
                const uint32_t in10[3] = {p.c[0], p.c[1], p.c[2]};
                tegra_vic_matrix_apply(&outMatrix, in10, p.c);
            }
            const int inSurface = m->flips == VIC_MODEL_FLIP_SURFACE;
            const uint32_t wx = fx ? (inSurface ? sw - 1 - x : L + R - x) : x;
            const uint32_t wy = fy ? (inSurface ? sh - 1 - y : T + B - y) : y;
            if (wx >= sw || wy >= sh || wx >= lw || wy >= lh) {
                m->clipped++;
                continue;
            }
            // Memory: row wy, column wx; transposed, row wx, column wy.
            const uint64_t at = tr ? (uint64_t)wx * lh + wy : (uint64_t)wy * lw + wx;
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
                const uint32_t halfX = format != TEGRA_VIC_T_Y8___U8V8_N422R;
                const uint32_t halfY = format != TEGRA_VIC_T_Y8___U8V8_N422;
                if ((halfX && (wx & 1)) || (halfY && (wy & 1)))
                    continue;  // one CbCr pair for each 2x2, 2x1 or 1x2: the top-left written pixel's
                const uint32_t cx = halfX ? wx / 2 : wx;
                const uint32_t cy = halfY ? wy / 2 : wy;
                if (cx >= cw || cy >= ch) {
                    m->clipped++;
                    continue;
                }
                const uint64_t cat = tr ? (uint64_t)cx * ch + cy : (uint64_t)cy * cw + cx;
                uint8_t *cq = vic_model_at(m, outChroma + cat * 2, 2);
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
