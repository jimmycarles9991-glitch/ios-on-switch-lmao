// SPDX-License-Identifier: BSD-2-Clause-Patent
// VIC's colour stages for test/vic_model.h (TegraVICColor.h has the TRM's chain and fields). From the TRM: a slot's
// value is linearised by its DegammaMode curve, its gamut matrix (the MatrixStruct law of tegra_vic_matrix_apply,
// offsets in 1/256 of a 10-bit step) works on the linear values, ClipEnabled clips negative results, RegammaMode
// re-encodes the blended value. Not in the TRM, so assumed here: each curve is its standard's formula evaluated exactly
// on value / 1023 (Rec.2020's with the 10-bit system's constants), the linear values in between keep full precision
// (15.3.2 promises DirectX's srgb2linear and linear2srgb accuracy), and the regamma's result is rounded to the 10-bit
// value the output truncates to 8 bits.
#ifndef VIC_MODEL_COLOR_H
#define VIC_MODEL_COLOR_H
#include <math.h>
#include <stdint.h>
#include "../TegraVICConfig.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t degamma, clip, regamma, gamutOn;
    TegraVICMatrix gamut;
} VicModelColor;

static inline double vic_model_degamma(uint32_t mode, double v)
{
    switch (mode) {
    case TEGRA_VIC_GAMMA_SRGB:
        return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
    case TEGRA_VIC_GAMMA_REC709:
        return v < 0.081 ? v / 4.5 : pow((v + 0.099) / 1.099, 1 / 0.45);
    case TEGRA_VIC_GAMMA_REC2020:
        return v < 0.08145 ? v / 4.5 : pow((v + 0.0993) / 1.0993, 1 / 0.45);
    default:
        return v;
    }
}

static inline double vic_model_regamma(uint32_t mode, double l)
{
    switch (mode) {
    case TEGRA_VIC_GAMMA_SRGB:
        return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1 / 2.4) - 0.055;
    case TEGRA_VIC_GAMMA_REC709:
        return l < 0.018 ? l * 4.5 : 1.099 * pow(l, 0.45) - 0.099;
    case TEGRA_VIC_GAMMA_REC2020:
        return l < 0.0181 ? l * 4.5 : 1.0993 * pow(l, 0.45) - 0.0993;
    default:
        return l;
    }
}

static inline void vic_model_color(const uint8_t *cfg, const TegraVICMatrix *gamut, int gamutOn, VicModelColor *c)
{
    c->degamma = (uint32_t)TEGRA_VIC_GET(cfg, TEGRA_VIC_SLOTS, SLOT_DEGAMMA);
    c->clip = (uint32_t)TEGRA_VIC_GET(cfg, TEGRA_VIC_SLOTS, SLOT_CLIP_ENABLED);
    c->regamma = (uint32_t)TEGRA_VIC_GET(cfg, TEGRA_VIC_OUTPUT, OUT_REGAMMA);
    c->gamutOn = (uint32_t)gamutOn;
    c->gamut = *gamut;
}

static inline uint32_t vic_model_code10(double v)
{
    const double c = v < 0 ? 0 : v > 1 ? 1 : v;
    return (uint32_t)lround(c * 1023);
}

// A slot's pipeline value through degamma, gamut and regamma (one slot: the blend passes it through).
static inline void vic_model_color_slot(const VicModelColor *c, uint32_t c10[3])
{
    if (!c->degamma && !c->gamutOn && !c->regamma)
        return;
    double l[3];
    for (int i = 0; i < 3; i++)
        l[i] = vic_model_degamma(c->degamma, c10[i] / 1023.0);
    for (int r = 0; r < 3; r++) {
        double g = l[r];
        if (c->gamutOn) {
            const double one = (double)(1u << (c->gamut.shift + 8));
            g = (c->gamut.c[r][0] * l[0] + c->gamut.c[r][1] * l[1] + c->gamut.c[r][2] * l[2]) / one +
                c->gamut.c[r][3] / (256.0 * 1023);
        }
        if (c->clip && g < 0)
            g = 0;
        c10[r] = vic_model_code10(vic_model_regamma(c->regamma, g > 1 ? 1 : g));
    }
}

// The background is in the blend's (linear) space: only the regamma acts on it.
static inline void vic_model_color_background(const VicModelColor *c, uint32_t c10[3])
{
    if (!c->regamma)
        return;
    for (int i = 0; i < 3; i++)
        c10[i] = vic_model_code10(vic_model_regamma(c->regamma, c10[i] / 1023.0));
}

#ifdef __cplusplus
}
#endif
#endif
