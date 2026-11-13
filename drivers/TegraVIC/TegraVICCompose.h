// SPDX-License-Identifier: BSD-2-Clause-Patent
// One VIC 4.0 job composing up to TEGRA_VIC_COMPOSE_MAX_SLOTS sources, bottom first, over opaque black into one RGB
// surface: the display's overlays as one plane (the engine's slots, which tegra_vic_build leaves at slot 0 alone).
// Plain C, header only, beside TegraVICConfig.h; test/tegra_vic_compose_test.cpp holds every field to libdrm's vic40.h.
//
// Sources:
//   averne's FFmpeg nvtegra libavutil/clb0b6.h (NVIDIA, MIT): slot b's surfaces at SET_SURFACE0_LUMA_OFFSET(b) =
//   0x400 + 0x60 b, CHROMA_U 0x404 + 0x60 b; Ryujinx's VicRegisters (SetSurfacexSlotx[slot][surface]) agrees, and the
//   eight slots end at 0x700, SET_PICTURE_INDEX.
//   libdrm tests/tegra/vic40.h (NVIDIA, MIT): BlendingSlotStruct's bits (AlphaK1 0..9, AlphaK2 16..25, the four
//   MatchSelects at 32, 36, 40 and 44).
//   NVIDIA open-gpu-doc classes/video/vic_ceb6_types.h and the T18x hwinc vic_enums.h (VIC 4.1): out = srcFactC src +
//   dstFactC dst per slot, dst what the slots below composed, and the select codes below. VIC 4.0 has the same bits;
//   that it has the same codes is what probe gpu.x1.vic_compose (SwitchVICComposeProbe.h) measures.
// The composite is what DC A's windows would have scanned: each slot's Y'CbCr goes to RGB through its own matrix and
// nothing else, so no transfer or gamut step runs and the slots' transfer and primaries are not compared.
#ifndef TEGRA_VIC_COMPOSE_H
#define TEGRA_VIC_COMPOSE_H
#include "TegraVICConfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TEGRA_VIC_COMPOSE_MAX_SLOTS        8u
#define TEGRA_VIC_SLOT_LUMA_METHOD(slot)   (TEGRA_VIC_M_SURFACE0_SLOT0_LUMA + 0x60u * (slot))
#define TEGRA_VIC_SLOT_CHROMA_METHOD(slot) (TEGRA_VIC_M_SURFACE0_SLOT0_CHROMA_U + 0x60u * (slot))

enum {
    SLOT_BLEND_K1_BIT = 0,
    SLOT_BLEND_K1_WIDTH = 10,
    SLOT_BLEND_K2_BIT = 16,
    SLOT_BLEND_K2_WIDTH = 10,
    SLOT_BLEND_SRC_C_BIT = 32,
    SLOT_BLEND_SRC_C_WIDTH = 3,
    SLOT_BLEND_DST_C_BIT = 36,
    SLOT_BLEND_DST_C_WIDTH = 3,
    SLOT_BLEND_SRC_A_BIT = 40,
    SLOT_BLEND_SRC_A_WIDTH = 3,
    SLOT_BLEND_DST_A_BIT = 44,
    SLOT_BLEND_DST_A_WIDTH = 3,
};

enum {
    TEGRA_VIC_SRC_C_K1 = 0,
    TEGRA_VIC_SRC_C_K1_SRC_ALPHA = 3,
    TEGRA_VIC_DST_C_ONE_MINUS_K1_SRC_ALPHA = 4,
    TEGRA_VIC_SRC_A_K1 = 0,
    TEGRA_VIC_DST_A_ONE_MINUS_K1_SRC_ALPHA = 1,
};

// How a slot covers what lies under it. Opaque: its alpha is the slot's constant (PlanarAlpha), every Y'CbCr source.
// Premultiplied: colour already carries its alpha (Core Animation's surfaces, DC A's own blend for an RGB window).
// Straight: colour is multiplied by alpha here.
enum {
    TEGRA_VIC_BLEND_OPAQUE = 0,
    TEGRA_VIC_BLEND_PREMULTIPLIED = 1,
    TEGRA_VIC_BLEND_STRAIGHT = 2,
};

typedef struct {
    const TegraVICImage *source;
    uint64_t srcX, srcY, srcW, srcH;  // 16.16
    uint32_t dstX, dstY, dstW, dstH;  // pixels, in the destination surface
    uint32_t blend;                   // TEGRA_VIC_BLEND_*
    uint32_t alpha10;                 // the slot's global alpha (K1), 1023 for 1.0
    uint32_t nearest;
} TegraVICComposeSlot;

typedef struct {
    uint32_t x, y, width, height;
} TegraVICRect;

static inline uint32_t tegra_vic_compose_methods(const TegraVICComposeSlot *slots, uint32_t count,
                                                 const TegraVICImage *dst)
{
    uint32_t n = 6 + (dst->format ? dst->format->planes - 1u : 0u);
    for (uint32_t i = 0; i < count; i++)
        n += slots[i].source && slots[i].source->format ? slots[i].source->format->planes : 0u;
    return n;
}

static inline const char *tegra_vic_compose_check_slot(const TegraVICComposeSlot *s, const TegraVICImage *dst,
                                                       const TegraVICTraits *t)
{
    const TegraVICImage *src = s->source;
    if (!src || !src->format)
        return "slot without a source";
    if (src->format->packed && !t->packedYcc)
        return "packed 4:2:2 this engine has not proven";
    if (src->format->planes > 2)
        return "three-plane slot source";
    const char *why = tegra_vic_check_image(src, tegra_vic_pitch_align(t, true));
    if (why)
        return why;
    if (src->blockLinear && (!t->blockLinearIn || !(src->format->subY || src->format->vic == TEGRA_VIC_T_L8)))
        return "block-linear source this engine has not proven";
    if (src->format->kind != TEGRA_VIC_KIND_RGB) {
        if (!(t->yccIn & TEGRA_VIC_YCC_IN_RGB))
            return "4:2:0 conversion this engine has not proven";
        if (src->standard > TEGRA_VIC_YCC_2020)
            return "YCbCr matrix requires color conversion outside VIC's contract";
    }
    if (s->blend > TEGRA_VIC_BLEND_STRAIGHT || s->alpha10 > 1023)
        return "slot blend outside opaque, premultiplied and straight";
    if (s->blend != TEGRA_VIC_BLEND_OPAQUE && !src->format->alpha)
        return "alpha blend of a source without alpha";
    if ((s->blend != TEGRA_VIC_BLEND_OPAQUE || s->alpha10 != 1023) && !t->slotAlpha)
        return "slot alpha blending this engine has not proven";
    if (s->srcW < (1u << 16) || s->srcH < (1u << 16))
        return "source rectangle under one pixel";
    if (s->srcX + s->srcW > ((uint64_t)src->width << 16) || s->srcY + s->srcH > ((uint64_t)src->height << 16))
        return "source rectangle outside the source";
    if (!s->dstW || !s->dstH || s->dstX + (uint64_t)s->dstW > dst->width || s->dstY + (uint64_t)s->dstH > dst->height)
        return "slot rectangle outside the destination";
    if (((uint64_t)s->dstW << 16) > s->srcW * TEGRA_VIC_MAX_UPSCALE ||
        ((uint64_t)s->dstH << 16) > s->srcH * TEGRA_VIC_MAX_UPSCALE)
        return "upscale past 16x";
    if (s->srcW > ((uint64_t)s->dstW << 16) * TEGRA_VIC_MAX_DOWNSCALE ||
        s->srcH > ((uint64_t)s->dstH << 16) * TEGRA_VIC_MAX_DOWNSCALE)
        return "downscale past 16x";
    return NULL;  // NOLINT(modernize-use-nullptr): C too
}

static inline void tegra_vic_compose_put_slot(uint8_t *c, uint32_t index, const TegraVICComposeSlot *s,
                                              const TegraVICTraits *t)
{
    const uint32_t slot = TEGRA_VIC_SLOTS + index * TEGRA_VIC_SLOT_BYTES;
    const TegraVICImage *src = s->source;
    const TegraVICFormat *sf = src->format;
    TEGRA_VIC_SET(c, slot, SLOT_ENABLE, 1);
    TEGRA_VIC_SET(c, slot, SLOT_CURRENT_FIELD, 1);
    const uint32_t filter = s->nearest || !t->bilinear ? TEGRA_VIC_FILTER_1TAP : TEGRA_VIC_FILTER_2TAP;
    TEGRA_VIC_SET(c, slot, SLOT_FILTER_X, filter);
    TEGRA_VIC_SET(c, slot, SLOT_FILTER_Y, filter);
    TEGRA_VIC_SET(c, slot, SLOT_SOFT_CLAMP_LOW, 0);
    TEGRA_VIC_SET(c, slot, SLOT_SOFT_CLAMP_HIGH, 1023);
    TEGRA_VIC_SET(c, slot, SLOT_PLANAR_ALPHA, 1023);
    TEGRA_VIC_SET(c, slot, SLOT_CONSTANT_ALPHA, s->blend == TEGRA_VIC_BLEND_OPAQUE);
    TEGRA_VIC_SET(c, slot, SLOT_SOURCE_LEFT, s->srcX);
    TEGRA_VIC_SET(c, slot, SLOT_SOURCE_RIGHT, s->srcX + s->srcW - (1u << 16));
    TEGRA_VIC_SET(c, slot, SLOT_SOURCE_TOP, s->srcY);
    TEGRA_VIC_SET(c, slot, SLOT_SOURCE_BOTTOM, s->srcY + s->srcH - (1u << 16));
    TEGRA_VIC_SET(c, slot, SLOT_DEST_LEFT, s->dstX);
    TEGRA_VIC_SET(c, slot, SLOT_DEST_RIGHT, s->dstX + s->dstW - 1);
    TEGRA_VIC_SET(c, slot, SLOT_DEST_TOP, s->dstY);
    TEGRA_VIC_SET(c, slot, SLOT_DEST_BOTTOM, s->dstY + s->dstH - 1);

    const uint32_t surf = slot + TEGRA_VIC_SLOT_SURFACE;
    uint32_t inH = 0;
    uint32_t inV = 0;
    tegra_vic_chroma_loc(sf, src->chromaSite, 0, &inH, &inV);
    const bool bl = src->blockLinear != 0;
    TEGRA_VIC_SET(c, surf, SURF_FORMAT, sf->vic);
    TEGRA_VIC_SET(c, surf, SURF_CHROMA_LOC_HORIZ, inH);
    TEGRA_VIC_SET(c, surf, SURF_CHROMA_LOC_VERT, inV);
    TEGRA_VIC_SET(c, surf, SURF_BLK_KIND, bl ? TEGRA_VIC_BLK_16Bx2 : TEGRA_VIC_BLK_PITCH);
    TEGRA_VIC_SET(c, surf, SURF_BLK_HEIGHT, bl ? src->blockHeightLog2 : 0);
    TEGRA_VIC_SET(c, surf, SURF_CACHE_WIDTH, bl ? TEGRA_VIC_CACHE_32Bx8 : TEGRA_VIC_CACHE_64Bx4);
    TEGRA_VIC_SET(c, surf, SURF_WIDTH, src->width - 1);
    TEGRA_VIC_SET(c, surf, SURF_HEIGHT, src->height - 1);
    TEGRA_VIC_SET(c, surf, SURF_LUMA_WIDTH, src->pitch[0] / sf->bpe[0] - 1);
    TEGRA_VIC_SET(c, surf, SURF_LUMA_HEIGHT, (bl ? src->rows[0] : src->height) - 1);
    TEGRA_VIC_SET(c, surf, SURF_CHROMA_WIDTH, sf->planes > 1 ? src->pitch[1] / sf->bpe[1] - 1 : 16383);
    TEGRA_VIC_SET(c, surf, SURF_CHROMA_HEIGHT,
                  sf->planes > 1 ? (bl ? src->rows[1] : tegra_vic_plane_rows(sf, src->height, 1)) - 1 : 16383);

    if (sf->kind != TEGRA_VIC_KIND_RGB) {
        TegraVICMatrix m;
        tegra_vic_matrix_ycc_to_rgb(src->standard, sf->kind == TEGRA_VIC_KIND_YCC_FULL, &t->in, &m);
        tegra_vic_matrix_put(c, slot + TEGRA_VIC_SLOT_MATRIX, &m);
    }

    const uint32_t blend = slot + TEGRA_VIC_SLOT_BLEND;
    TEGRA_VIC_SET(c, blend, SLOT_BLEND_K1, s->alpha10);
    TEGRA_VIC_SET(c, blend, SLOT_BLEND_SRC_C,
                  s->blend == TEGRA_VIC_BLEND_STRAIGHT ? TEGRA_VIC_SRC_C_K1_SRC_ALPHA : TEGRA_VIC_SRC_C_K1);
    TEGRA_VIC_SET(c, blend, SLOT_BLEND_DST_C, TEGRA_VIC_DST_C_ONE_MINUS_K1_SRC_ALPHA);
    TEGRA_VIC_SET(c, blend, SLOT_BLEND_SRC_A, TEGRA_VIC_SRC_A_K1);
    TEGRA_VIC_SET(c, blend, SLOT_BLEND_DST_A, TEGRA_VIC_DST_A_ONE_MINUS_K1_SRC_ALPHA);
}

// Composes slots[0] (bottom) .. slots[count - 1] (top) into dst's `target` rectangle, black and opaque wherever no slot
// lies. `configAddr`, `filterAddr` as tegra_vic_build. Returns 0, or why the job is refused (nothing is written then).
static inline const char *tegra_vic_compose_build(const TegraVICComposeSlot *slots, uint32_t count,
                                                  const TegraVICImage *dst, const TegraVICRect *target,
                                                  const TegraVICTraits *t, uint64_t configAddr, uint64_t filterAddr,
                                                  TegraVICJob *job)
{
    if (!slots || !count || count > TEGRA_VIC_COMPOSE_MAX_SLOTS)
        return "slot count outside 1..8";
    if (count > 1 && !t->multiSlot)
        return "multi-slot composition this engine has not proven";
    if (!dst->format || dst->format->kind != TEGRA_VIC_KIND_RGB)
        return "composition into a format other than RGB";
    if (dst->blockLinear && !t->blockLinearOut)
        return "block-linear destination this engine has not proven";
    const char *why = tegra_vic_check_image(dst, tegra_vic_pitch_align(t, false));
    if (why)
        return why;
    if (!target->width || !target->height || target->x + (uint64_t)target->width > dst->width ||
        target->y + (uint64_t)target->height > dst->height)
        return "target rectangle outside the destination";
    if ((configAddr | filterAddr) & 0xff)
        return "configuration not 256-byte aligned";
    for (uint32_t i = 0; i < count && !why; i++)
        why = tegra_vic_compose_check_slot(&slots[i], dst, t);
    if (why)
        return why;
    if (tegra_vic_compose_methods(slots, count, dst) > TEGRA_VIC_MAX_METHODS)
        return "more surface methods than one job holds";

    memset(job, 0, sizeof(*job));
    uint8_t *c = job->config;
    TEGRA_VIC_SET(c, TEGRA_VIC_PIPE, PIPE_DOWNSAMPLE_HORIZ, 4);
    TEGRA_VIC_SET(c, TEGRA_VIC_PIPE, PIPE_DOWNSAMPLE_VERT, 4);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_ALPHA_FILL_MODE, TEGRA_VIC_ALPHA_OPAQUE);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_ALPHA, 1023);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_LEFT, target->x);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_RIGHT, target->x + target->width - 1);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_TOP, target->y);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_BOTTOM, target->y + target->height - 1);

    const TegraVICFormat *df = dst->format;
    const bool blOut = dst->blockLinear != 0;
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_FORMAT, df->vic);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_BLK_KIND, blOut ? TEGRA_VIC_BLK_16Bx2 : TEGRA_VIC_BLK_PITCH);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_BLK_HEIGHT, blOut ? dst->blockHeightLog2 : 0);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_WIDTH, dst->width - 1);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_HEIGHT, dst->height - 1);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_WIDTH, dst->pitch[0] / df->bpe[0] - 1);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_HEIGHT, (blOut ? dst->rows[0] : dst->height) - 1);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_WIDTH, 16383);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_HEIGHT, 16383);

    uint64_t pixels = (uint64_t)target->width * target->height;
    for (uint32_t i = 0; i < count; i++) {
        tegra_vic_compose_put_slot(c, i, &slots[i], t);
        pixels += (slots[i].srcW >> 16) * (slots[i].srcH >> 16);
    }

    uint32_t n = 0;
    const uint32_t methods[] = {TEGRA_VIC_M_SET_APPLICATION_ID, TEGRA_VIC_M_SET_CONTROL_PARAMS,
                                TEGRA_VIC_M_SET_CONFIG_STRUCT_OFFSET, TEGRA_VIC_M_SET_FILTER_STRUCT_OFFSET,
                                TEGRA_VIC_M_OUTPUT_LUMA};
    const uint32_t values[] = {
        1, (TEGRA_VIC_CONFIG_BYTES / 16u) << 16 | TEGRA_VIC_CONTROL_GPTIMER_ON | TEGRA_VIC_CONTROL_FALCON_CONTROL,
        (uint32_t)(configAddr >> 8), (uint32_t)(filterAddr >> 8), (uint32_t)(dst->base[0] >> 8)};
    for (uint32_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {  // NOLINT(modernize-loop-convert): C too
        job->method[n] = methods[i];
        job->value[n++] = values[i];
    }
    for (uint32_t i = 0; i < count; i++) {
        const TegraVICImage *src = slots[i].source;
        job->method[n] = TEGRA_VIC_SLOT_LUMA_METHOD(i);
        job->value[n++] = (uint32_t)(src->base[0] >> 8);
        if (src->format->planes > 1) {
            job->method[n] = TEGRA_VIC_SLOT_CHROMA_METHOD(i);
            job->value[n++] = (uint32_t)(src->base[1] >> 8);
        }
    }
    job->method[n] = TEGRA_VIC_M_EXECUTE;
    job->value[n++] = TEGRA_VIC_EXECUTE_AWAKEN;
    job->count = n;
    job->pixels = pixels;
    return NULL;  // NOLINT(modernize-use-nullptr): C too
}

#ifdef __cplusplus
}
#endif
#endif
